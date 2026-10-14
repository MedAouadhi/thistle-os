# minimp3

From https://github.com/lieff/minimp3 at commit
`ea99364f61c14656440e8d77e9c233ccf3124633`, under CC0 (see `LICENSE`).
Only `minimp3.h` is taken; the Podcasts app decodes episodes with it.

One change: `mp3dec_decode_frame()` keeps a 16 KB scratch struct on the
stack. Defining `MINIMP3_SCRATCH_STORAGE` puts storage-class words in front
of it, and `pod_mp3.c` defines it as `static EXT_RAM_BSS_ATTR`, so the
scratch sits in PSRAM and the thread that decodes needs a small stack in
internal RAM, which runs out first. The decoder is then not reentrant: the
player is its only caller, from one thread.
