# Third-party components

Notepad Safe is distributed under GPLv3. The release package includes the full GPLv3 text. Exact versions, source locations and integrity identities are machine-locked in `dependencies.lock.json` and mirrored in `notepad-safe.spdx.json`.

| Component | Version | License | Upstream |
|---|---:|---|---|
| Qt | 6.11.1 | GPL-3.0-only | https://www.qt.io/ |
| QScintilla / Scintilla | 2.14.1 | GPL-3.0-only | https://riverbankcomputing.com/software/qscintilla/ |
| KeePassXC Core | 2.7.12 | GPL-2.0-only OR GPL-3.0-only | https://keepassxc.org/ |
| Botan | 3.12.0 | BSD-2-Clause | https://botan.randombit.net/ |
| Argon2 reference library | 20190702 | CC0-1.0 OR Apache-2.0 | https://github.com/P-H-C/phc-winner-argon2 |
| zlib | 1.3.2 | Zlib | https://zlib.net/ |
| PCRE2 | 10.47 | BSD-3-Clause WITH PCRE2-exception | https://www.pcre.org/ |

This summary is informational. The lock file and the upstream license texts remain authoritative for each component. A dependency update is not release-eligible until its version, source, integrity and license fields pass `tools/verify_supply_chain.ps1` and the generated SBOM is regenerated.
