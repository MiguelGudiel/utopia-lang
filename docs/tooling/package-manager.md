# Package Manager (yip)

`yip` is Utopia's package manager, modeled after Cargo and pub.

## Commands

```sh
utopia yip get <name>          # Resolve and download a package + dependencies
utopia yip add <name>          # Add a package to build.yaml
utopia yip publish             # Publish the current project as a tarball
utopia yip login               # Authenticate (GitHub OAuth flow)
```

## How packages work

- Packages are fetched from the registry server configured by `YIP_BASE_URL` (or `~/.utopia/credentials.json`).
- `get` queries `GET /index/<name>`, downloads the package archive, and unpacks it into the cache:

```
~/.utopia/cache/yip/packages/<name>/<version>/
```

- Each cached package is a complete Utopia project with its own `build.yaml`; imports use the `package:` prefix:

```utp
import "package:utpsdl/core";
import "package:engine/math/vector3";
```

## Declaring dependencies

```yaml
dependencies:
  - name: utpsdl
    version: "0.1.3"
    link: static
```

`link` controls whether the dependency is linked as a static archive or a shared library.

## Publishing

```sh
utopia yip publish
```

The current project is packed as a `tar.gz` and uploaded to `PUT /api/v1/packages/<name>/<version>`, with the dependency manifest sent in the `X-Yip-Deps` header.

## Authentication

`utopia yip login` starts a local OAuth flow with GitHub (a short-lived HTTP server listens on an ephemeral port and receives the callback). The token is stored in `~/.utopia/credentials.json`; the `YIP_TOKEN` environment variable is also honored.

## Environment

| Variable | Purpose |
| --- | --- |
| `YIP_BASE_URL` | Registry server base URL |
| `YIP_TOKEN` | Authentication token (alternative to login) |
