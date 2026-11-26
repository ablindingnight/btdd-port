# Playing online with a friend

Two PCs, one game: the **host** is player 1, the **joiner** is player 2. Nobody needs to open ports on a router.

Not needed for two players on **one PC**: plug in two gamepads (or share the keyboard) and press PLAY.
On the **same home network** (same Wi-Fi/router): the joiner can simply use **OR JOIN AN ADDRESS** with the
host PC's local address and port, e.g. `192.168.1.20:7845` (the host finds its IPv4 address in Windows
Settings -> Network -> properties). Join codes work there too.

## Before you start (both players)
- Same game files: the same Pond to Pavement build and the same `Battletoads-Double Dragon (USA).nes`.
- Same gameplay mods and cheats (Mods / Cheats pages). Picture settings (shaders, scale) can differ.
- Same **PPU** setting (Display page; leave it at the default if unsure).
- Have a chat open (Discord, text messages): you will send each other a short code.

## Host (player 1)
1. Open Pond to Pavement, go to **ONLINE** and press **HOST GAME** (or in the game: Esc -> Online 2-player ->
   Host game).
2. The game window shows a big **PLAYER 2 JOIN CODE** card (`BT1-.....`, in two groups so it is easy to read
   out). The code is already on your clipboard: paste it into the chat for your friend. The launcher's ONLINE page
   shows it too, with a COPY button. Lost it later? Esc -> Online 2-player -> **Show my join code**.
3. Wait. When it says **"connected: you are player 1"**, press Start on the title and play.

## Joiner (player 2)
1. Copy the host's code from the chat (the whole `BT1-...` text).
2. In Pond to Pavement go to **ONLINE**, press **PASTE**, then **JOIN** (or in the game: Esc -> Online 2-player
   -> Join with code, which reads the clipboard).
3. The window shows **"Connecting to player 1..."**, then **"connected: you are player 2"** and
   **"WAITING FOR THE HOST to press Start"** until the host starts the game.

Each of you plays with your own **player 1** keys or your own gamepad (gamepads work online).

## If it does not connect within about 20 seconds
1. After 20 s the joiner's window shows a **MY CODE: SEND IT TO PLAYER 1** card (also on the clipboard).
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
4. Or the host forwards **UDP port 7845** on the router to the host PC (router settings, "port forwarding"), and
   the joiner uses **OR JOIN AN ADDRESS** with `host-public-IP:7845` (the host can look up its public IP on a site
   like whatismyip.com). Once the port is forwarded this always works.

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
