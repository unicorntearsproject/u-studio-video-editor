# Audio

[Docs home](../README.md) › [User guide](README.md) › Audio

U-Stu is built for talking-head video: podcasts, interviews, streams
and the clips cut from them.

## Waveforms

Every clip with sound shows its waveform: across the whole clip for audio,
and along the bottom under the thumbnails for video. Waveforms are worked
out in the background, so a one-hour recording is ready in a few seconds
and never freezes the editor. You can turn them off in Settings.

## Sync recordings by their sound

Recorded the camera and a separate mic, or several cameras of the same
event? Line them up automatically:

1. Put the recordings on different tracks, roughly lined up (within about
   5 seconds of each other).
2. Select both clips.
3. Right-click the clip that should stay put and choose **Sync Tracks
   (Audio)**.

The other clip moves so the sound matches, to the exact frame, as one undo
step. If the match is weak or ambiguous, a clip has no sound, or the move
would land on another clip, the editor tells you and nothing moves.

## Split audio from video

Right-click a video clip with sound and choose **Split Audio**. Its sound
becomes a separate clip on the nearest audio track with room (a new track
is added if none has room). You can then move, trim or delete the two
halves independently. For example, keep the camera's picture and throw
away its scratch audio.

## Levels

- **Track volume:** right-click empty space on a track for its volume
  slider.
- **Mute a track:** right-click it and choose **Mute Track**. The track
  name strip then says "Muted".
- **Master volume:** use the volume control in the transport bar. It only
  changes what you hear, not the render.
- **Dissolves** cross-fade sound as well as picture
  ([Editing › Dissolves](editing-the-timeline.md#dissolves)).

## Output

Playback goes straight to your system's sound server (PipeWire or
PulseAudio). Renders use AAC, 48 kHz stereo ([Rendering](rendering.md)).

See also: [Editing on the timeline](editing-the-timeline.md) ·
[Troubleshooting](troubleshooting.md#no-sound)
