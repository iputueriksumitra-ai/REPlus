# WOW Cloud Hat Extension V3

V2 native execution path remains unchanged and is the confirmed working base.

## V3 UI behavior

Scene Clouds now exposes an explicit **Cloud Mode**:

- **As Recorded** - leaves/restores the cloud appearance recorded in the Rockstar Editor clip.
- **Live** - applies the custom **Cloud Hat** and **Cloud Opacity** selected below.

Cloud Hat and Cloud Opacity are intentionally disabled while Cloud Mode is As Recorded.
The last custom hat/opacity remain saved, so switching back to Live restores the chosen custom look immediately.

No `.clip` file is rewritten. The Cloud Hat remains state-driven; it is not reloaded every frame.
