# Playing online with a friend

Two PCs, one game: the **host** is player 1, the **joiner** is player 2. Nobody needs to open ports on a router.

## Before you start (both players)
- Same game files: the same Pond to Pavement build and the same `Battletoads-Double Dragon (USA).nes`.
- Same gameplay mods and cheats (Mods / Cheats pages). Picture settings (shaders, scale) can differ.
- Same **PPU** setting (Display page; leave it at the default if unsure).
- Have a chat open (Discord, text messages): you will send each other a short code.

## Host (player 1)
1. Open Pond to Pavement, go to **ONLINE** and press **HOST GAME** (or in the game: Esc -> Online 2-player ->
   Host game).
2. The game window shows **"Waiting for player 2. Your join code (copied): BT1-....."**. The code is already
   on your clipboard: paste it into the chat for your friend.
3. Wait. When it says **"connected: you are player 1"**, play.

## Joiner (player 2)
1. Copy the host's code from the chat (the whole `BT1-...` text).
2. In Pond to Pavement go to **ONLINE**, press **PASTE**, then **JOIN** (or in the game: Esc -> Online 2-player
   -> Join with code, which reads the clipboard).
3. The window shows **"Connecting to player 1..."** and then **"connected: you are player 2"**.

Each of you plays with your own **player 1** keys or gamepad.

## If it does not connect within about 20 seconds
1. The joiner's window also shows **your own code** (`If it does not connect, send them YOUR code: BT1-...`).
   Joiner: send that code to the host. Host: Esc -> Online 2-player -> **Paste player 2's code** (copy it
   first). Both games now try to reach each other at the same time, which gets through most home routers.
2. Still nothing: on either PC, Esc -> Online 2-player -> **Test my connection** (when not connected). It
   says whether the router supports UPnP and whether a public address was found.
   - "Public address: not found": something blocks UDP (a strict firewall or network). Allow the game in the
     Windows firewall prompt if one appeared, or try another network.
3. Some internet providers put many customers behind one address (mobile hotspots, some fibre plans); join codes
   cannot get through that. Then use a free VPN made for this: both install **Tailscale** (or ZeroTier), join the
   same network, and the joiner uses **OR JOIN AN ADDRESS** with the host's Tailscale address and `:7845`
   (for example `100.101.102.103:7845`).
4. Last resort: the host forwards **UDP port 7845** on the router to the host PC, and the joiner joins
   `host-public-IP:7845`.

## Messages you may see
| Message | Meaning |
|---|---|
| Cannot play together: different game ROM files / versions / PPU setting / mods | Make that setting the same on both PCs, then host and join again. |
| player 2 left the game / player 1 left the game | The other player closed the game or pressed Leave. |
| lost the connection: nothing from player N for 6 s | Their game stopped or the internet dropped. Esc -> Leave, then host and join again. |
| desync (the host gets **Resync** in the menu) | The two games drifted apart (should not happen). Host: Esc -> Resync. Both continue from the host's game. |

## Good to know
- The window title shows the ping. Above ~150 ms, raise **Input delay** to 2 (Online page; the host's value is
  used) for fewer corrections.
- Opening the menu pauses the game for both of you; the connection stays up.
- Privacy: the join code holds your public IP address; only send it to the person you play with. Making the code
  asks a STUN server (Google's by default) for that address; it sees your IP and nothing else. No game data
  leaves the two PCs.
