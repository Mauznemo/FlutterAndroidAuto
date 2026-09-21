# Releasing

For the maintainer. `dev/release.sh` does the whole thing; this explains what it does,
the two decisions behind it, and the one part that has to be set up by hand.

```bash
dev/release.sh                 # cut a release
dev/release.sh --dry-run       # every check, nothing pushed or published
dev/release.sh --skip-build    # skip the local native build check
dev/release.sh --skip-ci       # skip the build on CI. Only if CI itself is down
dev/release.sh --resume        # a publish died partway: finish the rest
```

It asks for the new version, shows what it is about to do, and makes you type the
version out before anything irreversible happens.

## The two decisions

**The three packages share one version number.** One version, one tag, one GitHub
release. The alternative, versioning each package on its own, is what Flutter's own
federated plugins do and it is more honest about what changed, but it means working out
three version numbers and three sets of cross constraints on every release. Lockstep
costs a version of a package that did not change, and buys never having to think about
it.

There is one real cost and it arrives with M12. Bumping
`android_auto_platform_interface` in lockstep means a major bump of the app-facing
package signals a breaking interface change to any *other* platform implementation,
which is exactly what a platform interface exists to avoid. With no third party
implementations that is theoretical. When the Android package lands, reconsider.

**Each package gets its own changelog.** A commit lands in a package's `CHANGELOG.md`
only if it touched that package's directory, so a note about the Linux bundle layout
does not appear in the platform interface's changelog. `tools/` counts as the Linux
package, since all of it exists to build or configure the native side. Commits that
touch nothing in `packages/` are in no changelog at all: how this repository runs its
own CI is not something a consumer consumes.

Only `feat:`, `fix:` and `refactor:` commits are collected. `chore:`, `docs:`, `test:`
and `revert:` are about the repository rather than the package's behaviour. That is
also why the commit style in [`CONTRIBUTING.md`](../CONTRIBUTING.md) is worth keeping
to: the changelog is generated from it, so a vague subject line becomes a vague
changelog entry.

The script offers to open the generated changelogs in `$EDITOR` before anything leaves
the machine. Take it. Generated entries are commit subjects, which are written for
someone reading `git log`, not always for someone deciding whether to upgrade.

## What it does, in order

Nothing before step 8 touches the network except to read.

1. **Refuses to start** unless: git, gh, flutter and python are present, `gh` is logged
   in, the branch is `main`, the working tree is clean, the aasdk submodule is checked
   out, and `main` is neither ahead of nor behind nor diverged from `origin/main`.
2. **Reads the current version** from the three pubspecs, refusing if they disagree.
3. **Takes the new version** and refuses anything that is not semver or is not greater
   than the current one. Refuses if the tag already exists, locally or on origin.
4. **Asks pub.dev what is published.** A failure to reach pub.dev is fatal rather than
   treated as "nothing published yet", because losing that check matters most exactly
   when the network is misbehaving.
5. **Rewrites the pubspecs**: the version, the cross package constraints, and
   `publish_to: none` out of the way.
6. **Generates the changelogs** and offers to open them.
7. **Checks**: `pub get`, that the workspace still resolves locally (below), `analyze`,
   the tests, a release build of the example, and that the bundle is self contained.
8. **Commits the bump locally**, then runs `dart pub publish --dry-run` on all three.
   The commit comes first because pub warns about a modified tree and treats its own
   warnings as a reason to stop, and this script has just rewritten six files. A
   failure here undoes the commit.
9. **Asks you to type the version**, then pushes.
10. **Builds on CI**, both architectures, and stops if it fails. See below.
11. **Publishes** in dependency order, tags, pushes the tag, opens the GitHub release.

## The build gate, and its fast path

Step 10 is the one that 0.1.0 did not have, and the reason it shipped unbuildable on
Ubuntu 24.04. The build used to be triggered *by* the GitHub release, which the script
creates after publishing, so it could only ever report on a release that had already
happened. It now runs between the push and the publish, and a failure stops everything.

**What a failure leaves behind:** nothing published, no tag, no GitHub release. Just
the version bump commit on `main`, which is harmless. Fix what broke, commit, push, and
run `dev/release.sh --resume` to release the same version once the build is green.

**The fast path.** A run that has already passed for the exact commit being released is
accepted immediately, without rebuilding. So this makes the release itself instant:

```bash
gh workflow run release.yml --ref main    # go and do something else
```

If there is no such run, the script dispatches one and waits, which is about eight
minutes for x86_64 and longer for aarch64. A run already in progress for that commit is
joined rather than duplicated.

`--skip-ci` exists and should be used only when CI itself is down. It is the only check
that compiles this code on a machine other than yours, which is precisely the class of
bug it caught: the release script's local build uses whatever compiler is installed
here, and one machine is not a portability test.

## The thing most likely to bite

The cross package dependencies are **caret constraints, not paths**:

```yaml
dependencies:
  android_auto_platform_interface: ^0.1.0
  android_auto_linux: ^0.1.0
```

pub refuses to publish a package that depends on a path, so they have to be
constraints. They still resolve to the sibling directories during development, because
a Dart workspace prefers its own members over pub.dev, but **only while the local
version satisfies the constraint**. Bump the versions to 0.2.0 and leave the
constraints at `^0.1.0`, and the next `flutter pub get` quietly starts resolving
against whatever pub.dev last published. Nothing says so. It looks like an edit not
taking effect.

That is why the script rewrites the constraints and the versions in one pass, and why
it asserts afterwards that all three still resolve to `packages/`. If that assertion
ever fires, the two have gone out of step.

## One warning that never goes away

`android_auto_linux` always reports:

```
* 1 checked-in file is ignored by a `.gitignore`.
  linux/third_party/aasdk/...
```

aasdk is vendored as a git submodule, its own `.gitignore` excludes its Docker build
environment and editor configuration, and those files are committed upstream anyway.
pub includes any checked-in file and warns when a `.gitignore` disagrees. A
`.pubignore` does not reach inside a submodule, so this cannot be silenced, only
understood. It is a warning, not an error, and pub.dev accepts the package.

The script tells errors and warnings apart and stops on either, but a warning only
needs a `y`. Read it rather than reflexively agreeing: a *new* warning appearing next
to this one is worth stopping for.

## If a publish fails partway

Publishing is the only step that cannot be undone, and it happens three times. If the
second one fails, the first is already permanent.

The script stops, says what went up, and leaves the bump commit pushed. Fix whatever
pub objected to, commit it, and run:

```bash
dev/release.sh --resume
```

which asks pub.dev what is missing at that version and publishes only that. It does not
bump anything again.

**There is no way to unpublish.** `dart pub retract` inside seven days marks a version
as retracted so pub stops resolving to it by default, but it stays on pub.dev forever.
A wrong version number is not a mistake you fix.

`--resume` does **not** re-run the build on CI. The build already passed for the commit
the version was released from, and a resume is finishing the upload of that same
version, so re-gating would mean a fresh fifteen minute build to publish one package.

### pub.dev lags its own uploads

Releasing 0.1.1 ran into this twice in a row and both were this script's fault.

pub.dev answers an upload with "it may take up-to 10 minutes before the new version is
available", and it means it. The wait for a published version to become resolvable used
to give up after 90 seconds and treat that as fatal, which stopped a release over a
slow cache after the package was already up. It now waits ten minutes and, if that is
not enough, warns and carries on.

The same lag then makes a resumed run read a package as missing when it is not, so the
script publishes it again and pub answers `Version 0.1.1 of package android_auto_linux
already exists`. That is the state the release wanted, so it is now treated as success
rather than as an error. Between them, the publish loop is safe to run as many times as
it takes.

## Moving the publish into CI

Today `dev/release.sh` publishes from the maintainer's machine, which needs a pub.dev
credential there. It does not have to stay that way, and the first release is the only
one that must work like that: pub.dev configures automated publishing on a package's
admin page, and a package has no admin page until it has been published once.

After the first release:

1. On each of the three packages' pub.dev pages, **Admin**, **Automated publishing**,
   enable publishing from GitHub Actions with repository `Mauznemo/FlutterAndroidAuto`
   and tag pattern `v{{version}}`. All three take the same pattern, because the
   versions are in lockstep and one tag covers them all.
2. In the repository, **Settings**, **Secrets and variables**, **Actions**,
   **Variables**: set `PUBLISH_FROM_CI` to `true`. That is what arms
   `.github/workflows/publish.yml`, which is otherwise inert.
3. Set `PUBLISH_FROM_CI=1` in `dev/release.sh`, so it tags and pushes rather than
   publishing, and lets the workflow do it.

pub.dev then verifies over OIDC that the request came from this repository at that tag,
and that the tag agrees with `pubspec.yaml`. No secret is stored in the repository or
on the machine.
