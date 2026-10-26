# Security policy

Linea is under active development and has no releases yet. Security fixes target the current `main` branch.

## Report a vulnerability

Use GitHub's [private vulnerability reporting form](https://github.com/Gondola-Bros-Entertainment/linea/security/advisories/new)
to report suspected vulnerabilities. Include the affected revision, compiler, platform, reproduction steps and expected
impact. A minimal SVG document or font file that reproduces the issue is especially useful.

Keep exploit details and sensitive data out of public issues and pull requests. Do not include files you cannot share.
The maintainers will review reports privately and coordinate fixes and disclosure through GitHub security advisories.

## Scope

Reports may cover the parts that read untrusted input, the SVG reader and the TrueType font reader, as well as
rasterization, the SVG writer, the PNG encoder and repository automation. Ordinary build failures and feature requests
belong in public issues.
