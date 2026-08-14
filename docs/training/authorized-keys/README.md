# Public keys authorised to reach the training boxes

Drop a **public** key here (`<machine>.pub`) and push; then run
`./scripts/enroll_lan_keys.sh` on the box you want it to reach. See
[`../ACCESS-hunter-pc.md`](../ACCESS-hunter-pc.md) §3.

A public key is not a secret, which is why this exchange is safe in a public
repository. **Never commit a private key** (`id_*` with no `.pub`), a password,
or a `known_hosts` file from a machine that has other hosts in it.

Enrolment is manual and per box: a key committed here is trusted by nothing until
someone runs the script.
