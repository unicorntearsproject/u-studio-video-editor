# 21 — Template sharing back end (AWS serverless, loose requirements)

[Docs home](../../README.md) › [v2 planning docs](README.md) › Template sharing back end

**Status:** loose requirements, 2026-09-27. The owner asked for "a
document which loosely defines what we would want from an AWS serverless
back end" so users can publish and share template packages
([doc 20](20-template-packages-and-sharing.md),
[ADR-020](adr/020-template-packages-and-sharing.md)). This is a brief for
whoever builds the service: what it must do and the shape we expect, not
a finished design. The service is a separate project and repository.

## Goals

- Users browse and search a catalogue of template packs, see previews,
  and download packs into U-Stu Video Editor.
- Signed-in publishers upload packs, update them with new versions, and
  withdraw them.
- Package files are **never publicly readable**. Downloads go through
  **short-lived signed URLs**, issued per request, so robots can't crawl
  or hot-link them (owner, 2026-09-27).
- Nothing the service accepts reaches other users until it has passed
  the same validation the client runs (doc 20), plus server-side checks.
- Cheap at low volume: pay per use, nothing always on.
- No tracking of users beyond what's needed for accounts, abuse
  prevention and download counts.

## Non-goals (for the first version)

- Payments or paid packs.
- Comments, social features or ratings beyond a simple report button and
  download counts.
- Hosting anything other than template packs: no video, no project files.
- An admin web UI beyond the minimum. The CLI or console is fine to
  begin with.

## Shape of the service

```text
 u-studio-share ──HTTPS──▶ CloudFront ──▶ API Gateway (HTTP API) ──▶ Lambda handlers
     │                         │                                     │
     │                         └── WAF (rate limits, bot control)     ├── DynamoDB (catalogue, publishers, reports)
     │                                                                ├── S3 "quarantine" bucket (uploads)
     └── signed URL ──▶ CloudFront (OAC) ──▶ S3 "packs" bucket (private)   └── S3 "packs" bucket (private)
                                                                          ▲
                S3 upload event ─▶ EventBridge ─▶ Step Functions: validate ─▶ scan ─▶ render previews ─▶ publish
```

| Need | Suggested AWS service | Notes |
|---|---|---|
| API | API Gateway HTTP API + Lambda | Small handlers, one per route; JSON |
| Edge, caching, signed downloads | CloudFront with Origin Access Control | Packs bucket private; **CloudFront signed URLs** (key group) valid ~5 minutes |
| Package storage | S3: `quarantine` (uploads, private, lifecycle 7 days) and `packs` (published, private, versioned) | No public ACLs or bucket policies anywhere; Block Public Access on |
| Preview images | S3 `previews` behind CloudFront, cacheable | Previews may be public-readable via CloudFront only; they are small and low value. Signing them too is an option |
| Catalogue metadata | DynamoDB (on-demand) | Packs, versions, publishers, reports, download counters |
| Search | DynamoDB GSIs on tag/category/updated, plus a simple title prefix index | OpenSearch Serverless only if volume needs it (it has a monthly floor cost) |
| Accounts | Cognito user pool, hosted UI | The client uses OAuth 2.0 authorization code + PKCE through the system browser; refresh tokens stored in the user's keyring by the helper |
| Upload | API issues an **S3 presigned PUT** (or POST policy) for the quarantine bucket | Size-limited via the policy (25 MB), content type fixed, single use, expires in minutes |
| Validation pipeline | EventBridge on the upload → Step Functions → Lambdas | Steps below |
| Malware and content checks | ClamAV in a container Lambda (or GuardDuty Malware Protection for S3); optional Rekognition moderation on images | Fonts and images are the only binary content |
| Preview rendering | A container Lambda with the titles renderer (`titlerender`, headless, Pango/Cairo) | Re-renders every template's preview so they can't be spoofed; also proves the pack renders |
| Abuse protection | AWS WAF on CloudFront: rate limits per IP on catalogue and download-URL routes, bot control managed rules, request size limits | Download-URL issuance is the throttle point |
| Infrastructure as code | AWS CDK (TypeScript) or SAM | One stack per environment (dev, prod) |
| Observability | CloudWatch logs and metrics, alarms to email | Logs keep no request bodies; IPs retained briefly for abuse only |
| Secrets | CloudFront signing key in Secrets Manager; rotated | |

## API (first version)

All JSON over HTTPS, versioned under `/v1`. Anonymous routes are rate
limited; publisher routes need a Cognito access token.

| Method and path | Who | Does |
|---|---|---|
| `GET /v1/packs?q=&tag=&sort=&cursor=` | anyone | Paged catalogue listing (id, title, author, tags, licence, latest version, preview URLs, download count) |
| `GET /v1/packs/{id}` | anyone | One pack: all published versions, description, template list with previews |
| `POST /v1/packs/{id}/versions/{version}/download` | anyone | Returns a **signed CloudFront URL** (expires ~5 min) and the package's SHA-256 and size; increments the download counter. POST, not GET, so crawlers following links don't mint URLs |
| `POST /v1/uploads` | publisher | Starts a publish: body has pack id and version from the manifest; returns a presigned PUT URL to quarantine and an upload id |
| `GET /v1/uploads/{uploadId}` | publisher | Pipeline status: `pending`, `validating`, `rejected` (with reasons), `in-review`, `published` |
| `DELETE /v1/packs/{id}/versions/{version}` | publisher (owner of the pack) | Withdraws a version; existing downloads keep working locally |
| `POST /v1/packs/{id}/report` | anyone (rate limited) | Report abuse or a licence problem; reason text |
| `GET /v1/me` / `PATCH /v1/me` | publisher | Publisher profile: display name, publisher slug |
| Admin (IAM-auth or separate API) | owner | Approve or reject in-review uploads, hide packs, ban publishers |

Errors use one JSON shape: `{ "error": code, "message": text }`.

## Publish pipeline

1. The client validates locally (doc 20) and calls `POST /v1/uploads`.
2. The client PUTs the archive to the presigned quarantine URL.
3. The S3 event starts a Step Functions execution:
   1. **Validate structure:** the same rules as the client (paths, entry
      types, limits, manifest schema, hashes, allowed types by magic
      bytes, `.ustitle` parse), implemented once as a shared library or
      container so the client and server can't drift.
   2. **Check identity:** the manifest's `id` belongs to this publisher
      (first publish claims it); `version` is higher than any published
      one; the service stamps the `publisher` field.
   3. **Scan:** malware scan; font licence is one of the allowed list;
      optional image moderation.
   4. **Render:** re-render every template's preview with the headless
      renderer. Failure to render rejects the pack.
   5. **Decide:** trusted publishers go straight to `published`; a new
      publisher's first pack (and anything flagged) goes to `in-review`
      (owner decision, doc 20 question 2).
   6. **Publish:** copy the archive to the private `packs` bucket under
      `packs/{id}/{version}/{sha256}.zip` (tar.gz packs keep their
      extension), write previews, write the catalogue entry, delete the
      quarantine object.
4. The client polls `GET /v1/uploads/{uploadId}` and shows the result.

## Data (DynamoDB, single table or a few; sketch)

| Entity | Key | Main attributes |
|---|---|---|
| Pack | `PACK#{id}` | publisher, title, description, tags, licence, latest version, created, updated, hidden |
| Version | `PACK#{id}` / `VER#{semver}` | S3 key, sha256, size, min-app-version, status, published at, template list |
| Publisher | `PUB#{sub}` | slug, display name, trusted flag, banned flag |
| Upload | `UPLOAD#{id}` | publisher, pack id, version, status, reasons, execution ARN, TTL |
| Report | `PACK#{id}` / `REPORT#{ts}` | reason, reporter (optional), status |
| Counters | on Version | downloads (atomic increment) |

GSIs for "by tag, newest", "by publisher" and "most downloaded".

## Security and privacy requirements

- No public S3 access anywhere; CloudFront with OAC is the only reader of
  the packs bucket, and only through signed URLs.
- Signed download URLs: short expiry (~5 minutes), scoped to one object,
  issued only by the rate-limited `download` route.
- Presigned uploads: single object, size cap, content-type fixed, short
  expiry, into quarantine only.
- The server never trusts the manifest: everything is re-validated.
- TLS everywhere; HSTS on the API domain.
- Account data minimal (Cognito sub, email for sign-in, display name).
  No analytics SDKs, no third-party trackers.
- Takedown path: a hidden pack disappears from listings and stops getting
  download URLs at once.
- Licence: every pack has an allowed SPDX licence; fonts must allow
  redistribution.
- Backups: S3 versioning on `packs`; DynamoDB point-in-time recovery.

## Cost notes (rough, low volume)

At hobby scale (thousands of downloads a month) everything above is pay
per use and should cost a few dollars a month. The notable fixed costs:

- WAF has a monthly base per web ACL plus per rule;
- a custom domain's Route 53 hosted zone;
- OpenSearch Serverless, if ever added, has a real monthly floor; that's
  why DynamoDB search comes first.

Set AWS Budgets alarms from day one.

## Decisions needed from the owner before building

1. The AWS account and region, and who operates it.
2. The domain for the API and downloads (with the `djunicorntears.com`
   decision).
3. Moderation policy (doc 20 question 2) and who reviews.
4. The allowed licence list (doc 20 question 3).
5. Anonymous downloads or sign-in required (doc 20 question 4; the
   recommendation is anonymous plus signed URLs plus WAF limits).
6. Whether the service lives in its own repository (recommended) and who
   builds it. The editor side only needs a mock of this API to be
   developed and tested (doc 20, T7).
