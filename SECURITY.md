# Security policy

## Reporting a vulnerability

Please do not disclose exploitable vulnerabilities in public issues or pull requests.

Use **Security > Advisories > Report a vulnerability** on this repository when private vulnerability reporting is available. If that option is unavailable, open an issue titled **Request private security contact**, without vulnerability details, and wait for a private reporting channel before sending them.

Include the application version, Windows version, affected component, minimal reproduction steps and expected impact. Use a synthetic project and relative paths where possible. Do not send real API credentials, an unredacted profile database, private source archives or other people's personal information. Redact logs and screenshots before sharing.

If a credential may have been exposed, revoke or rotate it with its provider promptly; deleting a local log does not revoke a key.

## Supported versions

Security fixes target the latest stable release. Update older releases and prereleases before reporting an issue that may already be fixed. There is no guaranteed response time or bug-bounty programme.

## Scope and precautions

Relevant areas include credential handling, workspace/scope boundaries, provider tool execution, proposal validation, verification scratch directories and Apply. Model output and project files are untrusted inputs.

Build and verification tools execute project-controlled instructions. Review unfamiliar projects before configuring or building them; verification is not a sandbox for hostile code. Keep backups and review proposed changes before Apply.
