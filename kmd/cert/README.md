# Test-signing certificate (not a secret)

Self-signed code-signing certificate for loading loophole kernel drivers in guests booted with
`bcdedit /set testsigning on`. It carries no trust outside the VMs that import it, which
loophole-setup.exe does. Never import it on a real machine.

- `loophole-test.crt` / `.cer`: certificate (PEM / DER)
- `loophole-test.key`: private key (PEM, unencrypted)
- `loophole-test.pfx`: PKCS#12 for `signtool` in the guest, password `loophole`
