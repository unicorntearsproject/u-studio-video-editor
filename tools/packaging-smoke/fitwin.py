# Resize the app's top-level window to fill the Xvfb screen (no WM needed).
import sys, time
from Xlib import display, X
d = display.Display()
root = d.screen().root
W, H = d.screen().width_in_pixels, d.screen().height_in_pixels
def walk(w):
    name = ''
    try:
        prop = w.get_full_property(d.intern_atom('_NET_WM_NAME'), d.intern_atom('UTF8_STRING'))
        name = prop.value.decode('utf-8', 'replace') if prop else (w.get_wm_name() or '')
    except Exception:
        pass
    try:
        cls = w.get_wm_class() or ()
    except Exception:
        cls = ()
    attrs = w.get_attributes()
    # The window title ends "— U-Stu" since 0.51.0-beta.2 ("— u Studio" before);
    # the class (com.ustudio.VideoEditor / u-studio-video-editor) matches both.
    if attrs.map_state == X.IsViewable and ('U-Stu' in name or 'u Studio' in name or any('u-studio' in c.lower() or 'videoeditor' in c.lower() for c in cls)):
        return w
    for c in w.query_tree().children:
        r = walk(c)
        if r: return r
    return None
for _ in range(100):
    win = walk(root)
    if win: break
    time.sleep(0.2)
if not win:
    print('no window'); sys.exit(1)
win.configure(x=0, y=0, width=W, height=H)
d.sync()
print('resized', W, H)
