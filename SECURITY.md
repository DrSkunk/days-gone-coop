# Security

The UDP/LAN transport is intended only for a **trusted local network**. It is
neither encrypted nor authenticated, so any reachable peer can send it game
state. Never forward its port to the Internet. Steam transport uses Steam's
authenticated and encrypted networking; Internet/NAT traversal has not been
independently verified.

To report a vulnerability, for example a packet that crashes the game or
corrupts memory, use
[GitHub's private vulnerability reporting](https://github.com/DrSkunk/theys-gone/security/advisories/new)
instead of a public issue.
