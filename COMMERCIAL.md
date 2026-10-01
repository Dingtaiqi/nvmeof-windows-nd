# Commercial Licensing

This repository is licensed under the **GNU Affero General Public License v3.0 or later**
(see [LICENSE](LICENSE)).

**AGPL does not prohibit commercial use.** Using it inside a company, making money with it,
running it as a service — all allowed, free of charge. What it requires is *giving back*: if
you distribute this project or a derivative work — **including offering it over a network** —
you must make the complete corresponding source available under the same license
(section 13 is the network clause).

So there is exactly one situation that needs the author:

> **You need to stay closed source.** Embedding it in a proprietary product, or building a
> closed-source service on top of it without publishing your changes.

In that case, a commercial license — free of the AGPL's obligations — is available.

## When a commercial license is needed

| How you use it | Do you need to buy one? |
|---|---|
| Personal study, research, experiments, hobby projects | No |
| Internal company use (unmodified, or modified but used only internally and not offered to third parties) | No |
| Shipping it in a product or service **and** publishing your modifications under the AGPL | No |
| Embedding it in a closed-source product, or running a closed-source SaaS **without** publishing your changes | **Yes** |
| Merging the code into a project whose license is incompatible with the AGPL (see below) | **Yes** |

An internal evaluation does not need a license up front — come and talk before you ship
something closed.

## How to get one

Open an issue titled `Commercial license` and describe:

- who the licensee is (company/organisation)
- what for (internal tool / product component / external service / customer delivery)
- scale (deployments, users, whether it ships with a product)
- whether you need to modify the source, and whether you really need to keep it closed

Terms can be per-project or annual; attribution of derivative works and whether upstream
contributions are required are both negotiable.

## License compatibility (two traps)

1. **The Linux kernel headers under `ref/` are GPL-2.0** (`linux_nvme.h`, `linux_nvme_rdma.h`).
   They are read for comparison only and are **never compiled**; `run_xref.ps1` merely
   compares their constants with ours. **GPL-2.0-only and AGPL-3.0 are incompatible** — do not
   merge code from those files into this project. If you genuinely need to, the whole project
   would have to become GPL-2.0 instead (still open source, but it gives up AGPL section 13).
2. **Linking the vendor NetworkDirect library is fine.** `ndutil`/NDSPI is not a copyleft
   license, and neither static nor dynamic linking to it affects your choice for this project.

## Why it is set up this way

This project can become a product more or less as it stands: on Windows it already mounts a
remote 1 TB disk as a live network drive (GPT/NTFS readable, write-protected, with the
drive's own SMART counters proving zero writes), interoperates **both ways** with Linux's
`nvmet` / `nvme-cli`, and implements in-band DH-HMAC-CHAP authentication
(ffdhe2048 + hmac(sha256), both directions, measured against a real Linux peer).

The author is not against commercial use; the objection is to "take it, close it, tweak it,
sell it as your own". The AGPL blocks exactly that: either give back, or buy a license.
Both routes are open.
