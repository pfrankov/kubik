# npm channel releases

The public package is `openclaw-kubik`; the OpenClaw channel ID is `kubik`.
Users install it with `openclaw plugins install openclaw-kubik`.
Package documentation is English. Firmware versions are independent; the npm
README states the compatible firmware and OpenClaw versions.

`.github/workflows/publish-npm.yml` runs after successful Acceptance on `main`.
It publishes the exact tested commit only while that commit remains main HEAD.
Manual dispatch also requires successful Acceptance for that same commit.
Acceptance owns the complete checks and their tool dependencies; the publication
job validates and packs that accepted source without repeating the test suite.
A superseded commit is skipped. An already published version is accepted only
when the packed tarball integrity matches; changed contents require a version bump.
Network errors and mismatched contents fail the workflow.
This includes packaged documentation and license notices: updating the packaged `NOTICE`
requires a new package version even when the JavaScript is unchanged.

Publishing uses npm Trusted Publishing (GitHub OIDC), not a stored npm token.
The publisher is `pfrankov/kubik`, workflow `publish-npm.yml`, with the npm publish
operation allowed. First publish through the maintainer's npm login to create
the package. Then explicitly configure its GitHub Trusted Publisher in npm
settings; later releases use the workflow. The authenticated bootstrap is:

```sh
cd openclaw-kubik
npm login
node scripts/publish.mjs
npx --yes npm@12.2.0 trust github openclaw-kubik --repo pfrankov/kubik --file publish-npm.yml --allow-publish --yes
npx --yes npm@12.2.0 trust list openclaw-kubik
```

Approve the npm login/publish challenge in your browser when requested. The trust
command grants publication only to the named GitHub workflow, not another
repository. No npm publishing token is saved in GitHub secrets.

Before changing package contents, bump `openclaw-kubik/package.json` and its
lockfile, run `npm run sync:manifest`, and update compatibility and CHANGELOG.
Run `python3 tools/accept.py`, then commit and push to main. Never reuse an
immutable npm version for different contents. Do not put npm credentials in Git.
