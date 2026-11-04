# 04: Video in calls on Linux

**What to build:** in a call between two Linux companion apps either side switches its camera on or off at any time, from the first ring. A conversation with a video-capable contact has a Video call button (absent for the T-Deck and older apps); a call offered with the camera on shows "Video call" on the ring screen and in its notification, with Answer with video, Answer (camera stays off) and Decline; a missed one is "Missed video call". The call screen shows the other side's video full size, one's own in a corner, the camera switch (only on a WebRTC call), Switch camera when there is more than one camera, and a tile saying Camera off, Camera paused or No camera. A machine with no camera still sees the other side. Video adapts to the path, capped at 1280x720, 30 fps; switching never renegotiates. See `../spec.md`, user stories 1 to 23.

**Blocked by:** 03

**Status:** ready-for-agent

- [ ] The Video call button by capability; a call started from it rings with `video` and the caller's camera on
- [ ] The ring screen's three buttons for a `video` offer, two otherwise; a plain Answer leaves the camera off; the camera never turns on without a tap
- [ ] The camera switch replaces the track and sends `call_camera`; the other side's tile follows `on`, `off`, `paused` and `none`; Switch camera cycles cameras when there are two or more
- [ ] No camera on one side: it sends `none` and still shows the other side's video
- [ ] Capture and sending capped at 1280x720, 30 fps
- [ ] Notifications say "Video call from" and "Missed video call" when the offer had `video`
- [ ] While a camera is on, the app starts no child process, unless ticket 02 showed its short-lived ones do no harm
- [ ] The app's call tests, with the fake media engine: the button present or absent by capability, the ring screen's buttons, the plain Answer, the switch, Switch camera with one and two cameras, the tile's states
- [ ] By hand, written here: a video call between two Linux machines, both ways, camera switched mid-call; one machine without a camera
