# Muse Gadget SDK chat

Apache-2.0; pinned `74a5e2d7fc895f109f83a9a1dbed705dbcd8b1ff`.
https://github.com/facebookincubator/muse-gadget-sdk

Native no-PSRAM voice-note and reply correlation implementation.
Port headers expose only the Link request operations used by this file.
Kubik adds a full-text accessor for its card UI and removes reply content
from diagnostic logs. Encoding/caption helpers are implemented in main.

Completed replies settle after 3 seconds without waiting for caption reading pace;
Kubik owns full-text pagination. Reply correlation/limits remain upstream.
The offline chat harness is upstream Apache-2.0, with local source/include paths.
Large chat buffers are allocated only by the native Muse worker; host agents
do not reserve their RAM. Start reports allocation failure explicitly.
