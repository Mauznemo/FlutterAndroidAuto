#!/usr/bin/env bash
# Cut a release: bump the three packages, tag it, publish to pub.dev, open a GitHub
# release.
#
#   dev/release.sh                 the real thing, with every gate
#   dev/release.sh --dry-run       do everything except commit, push, publish, release
#   dev/release.sh --skip-build    skip the local native build check
#   dev/release.sh --skip-ci       skip the build on CI. Only if CI itself is down
#   dev/release.sh --resume        a publish died partway: finish the ones still missing
#
# The three packages are versioned in lockstep, so there is one version number, one tag
# `vX.Y.Z` and one GitHub release. Each package still gets its own CHANGELOG.md, filled
# from the feat, fix and refactor commits that touched that package.
#
# ## Why this is paranoid
#
# Publishing to pub.dev cannot be undone. There is no unpublish; retracting inside seven
# days marks a version as retracted and leaves it there forever. A wrong version number
# is not a mistake you fix, it is one you live with. So nothing here is irreversible
# until every check has passed and the version has been typed out in full, and the two
# irreversible stretches, pushing and publishing, happen as late as possible and in the
# order that fails safest.
#
# ## The one thing to understand before changing it
#
# The cross package dependencies are caret constraints, not paths, because pub refuses
# to publish a package that depends on a path. They still resolve to the sibling
# directories during development, because a Dart workspace prefers its own members over
# pub.dev, but only while the local version satisfies the constraint. That is why the
# bump rewrites the constraints and the versions together, and why lockstep is what
# keeps it true. Break that and `flutter pub get` silently starts resolving against
# pub.dev, which looks like nothing at all until an edit stops taking effect.

set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SUPPORT="$REPO/dev/release_support.py"
cd "$REPO"

DRY_RUN=0
SKIP_BUILD=0
SKIP_CI=0
RESUME=0
# Flip to 1 once all three packages exist on pub.dev and automated publishing is
# configured for each of them. The script then tags and pushes and lets
# .github/workflows/publish.yml do the publishing over OIDC, so no pub.dev credential
# is ever needed on this machine. See docs/releasing.md.
PUBLISH_FROM_CI=${PUBLISH_FROM_CI:-0}

for arg in "$@"; do
  case "$arg" in
    --dry-run)    DRY_RUN=1 ;;
    --skip-build) SKIP_BUILD=1 ;;
    --skip-ci)    SKIP_CI=1 ;;
    --resume)     RESUME=1 ;;
    -h|--help)    sed -n '2,9p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *)            echo "unknown argument $arg" >&2; exit 1 ;;
  esac
done

FLUTTER="$(command -v flutter || echo "$HOME/snap/flutter/common/flutter/bin/flutter")"
DART="$(dirname "$FLUTTER")/dart"

bold()  { printf '\033[1m%s\033[0m\n' "$*"; }
step()  { printf '\n\033[1;34m==>\033[0m \033[1m%s\033[0m\n' "$*"; }
warn()  { printf '\033[1;33mwarning:\033[0m %s\n' "$*"; }
die()   { printf '\033[1;31merror:\033[0m %s\n' "$*" >&2; exit 1; }

# --------------------------------------------------------------------------------
# Gates. Nothing below this point writes anything.
# --------------------------------------------------------------------------------

step "Checking the machine and the repository"

for tool in git gh python3 "$FLUTTER" "$DART"; do
  command -v "$tool" >/dev/null 2>&1 || [ -x "$tool" ] \
    || die "$tool is not installed or not on PATH"
done

[ -f "$SUPPORT" ] || die "dev/release_support.py is missing"

gh auth status >/dev/null 2>&1 \
  || die "gh is not logged in. Run: gh auth login"

# Checked here rather than discovered at the publish, which happens after the push and
# after the point of no return. `dart pub publish` would open a browser and wait, in
# the middle of a release, which is the worst possible moment to find out.
#
# The path is the one `dart pub logout` names. If a future Dart moves it this gate
# false positives, and the cost of that is one harmless `dart pub login`.
if [ "$PUBLISH_FROM_CI" = "0" ]; then
  PUB_CREDENTIALS="${XDG_CONFIG_HOME:-$HOME/.config}/dart/pub-credentials.json"
  [ -f "$PUB_CREDENTIALS" ] || [ -f "${PUB_CACHE:-$HOME/.pub-cache}/credentials.json" ] \
    || die "not logged in to pub.dev, and this script publishes from here. Run:
         dart pub login
       That opens a browser once and stores a credential at
       $PUB_CREDENTIALS"
fi

BRANCH="$(git rev-parse --abbrev-ref HEAD)"
[ "$BRANCH" = "main" ] \
  || die "releases are cut from main, and this is '$BRANCH'"

[ -z "$(git status --porcelain)" ] \
  || die "the working tree is not clean. Commit or stash first, then run this again.
       A release has to be reproducible from what is committed, and pub publishes
       what is in the directory rather than what is in the commit."

[ -f "packages/android_auto_linux/linux/third_party/aasdk/CMakeLists.txt" ] \
  || die "the aasdk submodule is not checked out. Run:
       git submodule update --init --recursive"

step "Checking this branch is in step with the remote"

git fetch --quiet origin main
LOCAL="$(git rev-parse @)"
REMOTE="$(git rev-parse origin/main)"
BASE="$(git merge-base @ origin/main)"
if [ "$LOCAL" != "$REMOTE" ]; then
  if [ "$LOCAL" = "$BASE" ]; then
    die "main is behind origin/main. Pull first."
  elif [ "$REMOTE" = "$BASE" ]; then
    die "main is ahead of origin/main by $(git rev-list --count origin/main..@) commit(s).
       Push them first, so the release tag points at something the remote already has."
  else
    die "main and origin/main have diverged. Sort that out before releasing."
  fi
fi

# --------------------------------------------------------------------------------
# Version
# --------------------------------------------------------------------------------

CURRENT="$(python3 "$SUPPORT" current)"
LAST_TAG="$(git tag -l 'v*' --sort=-v:refname | head -1 || true)"

step "Version"
echo "  current version:  $CURRENT"
echo "  last release tag: ${LAST_TAG:-none, this would be the first}"
echo

if [ "$RESUME" = "1" ]; then
  NEW="$CURRENT"
  echo "  --resume: finishing the release of $NEW"
else
  read -r -p "  new version: " NEW
  [ -n "$NEW" ] || die "no version given"
  NEW="$(python3 "$SUPPORT" check "$NEW")"
fi

TAG="v$NEW"

if [ "$RESUME" = "0" ]; then
  git rev-parse "$TAG" >/dev/null 2>&1 \
    && die "the tag $TAG already exists locally. Pick another version, or delete it."
  git ls-remote --exit-code --tags origin "refs/tags/$TAG" >/dev/null 2>&1 \
    && die "the tag $TAG already exists on origin. That version has been released."
fi

# pub.dev is the only authority on what has been published, and publishing is the step
# that cannot be undone, so ask it rather than trusting the tags. A failure to reach it
# is fatal: assuming "nothing published yet" because the network was down is how a
# release ends up trying to republish a version that already exists.
step "Asking pub.dev what is already published"
PUBLISHED=""
while read -r package state value; do
  case "$state" in
    missing) echo "  $package: not on pub.dev yet" ;;
    has)     echo "  $package: $value is ALREADY PUBLISHED"
             PUBLISHED="$PUBLISHED $package" ;;
    latest)  echo "  $package: latest is $value" ;;
  esac
done < <(python3 "$SUPPORT" published "$NEW")

if [ -n "$PUBLISHED" ] && [ "$RESUME" = "0" ]; then
  die "version $NEW is already on pub.dev for:$PUBLISHED
       A published version can never be replaced. Choose a higher version.
       If a previous run died partway through publishing, use --resume."
fi

# --------------------------------------------------------------------------------
# Write: versions, constraints, changelogs. Still nothing pushed or published.
# --------------------------------------------------------------------------------

if [ "$RESUME" = "0" ]; then
  step "Rewriting pubspecs to $NEW"
  python3 "$SUPPORT" bump "$NEW"

  step "Generating changelogs from the commits since ${LAST_TAG:-the beginning}"
  python3 "$SUPPORT" changelog "$NEW" "${LAST_TAG:-}"

  echo
  bold "  Review what was generated before anything leaves this machine."
  read -r -p "  open the three changelogs in \$EDITOR now? [y/N] " edit
  if [ "$edit" = "y" ] || [ "$edit" = "Y" ]; then
    "${EDITOR:-nano}" packages/*/CHANGELOG.md
  fi
fi

# --------------------------------------------------------------------------------
# Checks
# --------------------------------------------------------------------------------

step "Resolving dependencies"
"$FLUTTER" pub get >/dev/null

# The caret constraints must still resolve to the sibling directories. If this breaks,
# development silently starts running against whatever pub.dev last published.
python3 "$SUPPORT" verify-local

step "Analyzing"
"$FLUTTER" analyze

step "Testing"
"$FLUTTER" test packages/*/test

if [ "$SKIP_BUILD" = "0" ]; then
  step "Building the example, release, and checking the bundle is self contained"
  (cd example && "$FLUTTER" build linux --release)

  bundle="example/build/linux/$(uname -m | sed 's/x86_64/x64/; s/aarch64/arm64/')/release/bundle"
  if find "$bundle" -xtype l | grep .; then
    die "the bundle contains dangling symlinks, see docs/packaging.md"
  fi
  if ls "$bundle/lib" | grep -E '^libaasdk|^libaap_protobuf'; then
    die "aasdk is being shipped beside the plugin again, see docs/packaging.md"
  fi
  echo "  bundle libraries: $(ls -1 "$bundle/lib" | tr '\n' ' ')"
else
  warn "skipping the native build check (--skip-build)"
fi

# The bump is committed before pub validates it, because `dart pub publish --dry-run`
# exits non zero on a *warning*, and "checked-in files are modified in git" is a warning
# this script would otherwise always trigger, having just rewritten six files.
# Committing first makes the tree clean, so every warning left is a real one. The commit
# is local until the confirmation below, and undo_commit puts it back.
if [ "$RESUME" = "0" ]; then
  step "Committing the bump locally"
  git add -A
  git commit -q -m "chore: bump version to $NEW"
  echo "  $(git rev-parse --short HEAD)  chore: bump version to $NEW"
fi

undo_commit() {
  # --soft, so the work is kept and staged rather than thrown away.
  [ "$RESUME" = "0" ] || return 0
  git reset -q --soft HEAD~1
  echo "  the bump commit was undone; your changes are still staged."
}

# pub exits 65 for a warning just as it does for an error, so the exit code alone
# cannot decide this. The difference matters: an error means pub.dev would refuse the
# package, a warning means it would take it and grumble.
#
# One warning is expected and permanent. android_auto_linux vendors aasdk as a git
# submodule, aasdk's own .gitignore excludes its Docker build environment and editor
# configuration, and those files are nonetheless committed upstream. pub includes any
# checked-in file and warns when a .gitignore disagrees. A .pubignore does not reach
# into a submodule, so this cannot be silenced from here, only understood.
validate_one() {
  local package="$1" out status=0
  out="$(cd "packages/$package" && "$DART" pub publish --dry-run 2>&1)" || status=$?
  printf '%s\n' "$out" | sed 's/^/    /'
  if printf '%s' "$out" | grep -qE 'validation found the following [0-9]* ?errors?:'; then
    return 2
  fi
  [ "$status" -eq 0 ] && return 0
  return 1
}

step "Validating all three packages against pub"
WARNED=""
for package in $(python3 "$SUPPORT" packages); do
  echo
  bold "  $package"
  set +e
  validate_one "$package"
  verdict=$?
  set -e
  case "$verdict" in
    0) ;;
    1) WARNED="$WARNED $package" ;;
    2) undo_commit
       die "pub.dev would REFUSE $package. Nothing has been pushed or published." ;;
  esac
done

if [ -n "$WARNED" ]; then
  echo
  warn "pub raised warnings, not errors, for:$WARNED"
  echo "  pub.dev would accept these. Read them above before going on."
  read -r -p "  continue anyway? [y/N] " proceed
  if [ "$proceed" != "y" ] && [ "$proceed" != "Y" ]; then
    undo_commit
    die "stopped at the warnings. Nothing has been pushed or published.
       Run 'git reset --hard HEAD && git clean -fd' to drop the bump entirely."
  fi
fi

# --------------------------------------------------------------------------------
# The point of no return
# --------------------------------------------------------------------------------

step "Ready"
cat <<SUMMARY
  version:   $CURRENT  ->  $NEW
  tag:       $TAG
  packages:  $(python3 "$SUPPORT" packages | tr '\n' ' ')

  What happens next, in order:
    1. push    the bump commit, already made locally, to origin
SUMMARY
if [ "$PUBLISH_FROM_CI" = "1" ]; then
  cat <<SUMMARY
    2. build   on CI, Linux on both architectures plus macOS and Windows, and stop
               here if any of them fails
    3. tag     $TAG and push it
    4. release on GitHub, which triggers the publish workflow over OIDC
SUMMARY
else
  cat <<SUMMARY
    2. build   on CI, Linux on both architectures plus macOS and Windows, and stop
               here if any of them fails
    3. publish all three to pub.dev, in dependency order    <-- CANNOT BE UNDONE
    4. tag     $TAG and push it
    5. release on GitHub
SUMMARY
fi

if [ "$DRY_RUN" = "1" ]; then
  echo
  bold "  --dry-run: stopping here. Nothing was pushed or published."
  undo_commit
  bold "  Run 'git reset --hard HEAD && git clean -fd' to drop the bump entirely."
  exit 0
fi

echo
warn "Publishing to pub.dev is permanent. A version cannot be replaced or removed."
read -r -p "  type the version ($NEW) to continue, anything else to abort: " confirm
if [ "$confirm" != "$NEW" ]; then
  undo_commit
  die "aborted. Nothing was pushed or published.
       Run 'git reset --hard HEAD && git clean -fd' to drop the bump entirely."
fi

# --------------------------------------------------------------------------------
# Commit and push. Done before publishing so that what is on pub.dev always exists on
# the remote too, rather than the other way round.
# --------------------------------------------------------------------------------

# Every build, on machines that are not this one, before anything is published: the
# native Linux build on both architectures, and the example on macOS and Windows, where
# it runs the simulator. They are one workflow run, so one watch covers them all.
#
# This is the gate 0.1.0 did not have. The build used to be triggered *by* the GitHub
# release, which this script creates after publishing, so it could only ever report on
# a release that had already happened. 0.1.0 went to pub.dev unbuildable on Ubuntu
# 24.04 and CI said so eight minutes later.
#
# The fast path matters as much as the gate. A run that has already passed for this
# exact commit is accepted at once, so `gh workflow run release.yml --ref main` while
# you do something else makes the release itself instant. Only when there is no such
# run does this start one and wait.
ci_run_for() {
  # Newest run of release.yml for $1 matching the jq filter in $2, or empty.
  gh run list --workflow=release.yml --limit 50 \
    --json databaseId,headSha,status,conclusion \
    --jq "[.[] | select(.headSha == \"$1\" and $2)] | first | .databaseId // empty" \
    2>/dev/null || true
}

ci_gate() {
  local sha="$1" run=""

  run="$(ci_run_for "$sha" '.conclusion == "success"')"
  if [ -n "$run" ]; then
    echo "  already green for ${sha:0:8} (run $run), not rebuilding"
    return 0
  fi

  # A run already going for this commit is joined rather than duplicated.
  run="$(ci_run_for "$sha" '.status != "completed"')"
  if [ -n "$run" ]; then
    echo "  joining the build already running for this commit"
  else
    echo "  no build has passed for this commit, starting one"
    gh workflow run release.yml --ref main
    local waited=0
    # The run does not exist the instant the dispatch returns.
    while [ -z "$run" ] && [ "$waited" -lt 90 ]; do
      sleep 5
      waited=$((waited + 5))
      run="$(ci_run_for "$sha" '.status != "completed"')"
    done
    [ -n "$run" ] || die "the build was dispatched but no run appeared for ${sha:0:8}
       after ${waited}s. Nothing has been published. Check the Actions tab, then:
         dev/release.sh --resume"
  fi

  echo "  watching run $run. x86_64 takes about eight minutes, aarch64 longer; macOS"
  echo "  and Windows build alongside and finish sooner."
  echo "  https://github.com/Mauznemo/FlutterAndroidAuto/actions/runs/$run"
  # --exit-status fails the shell when the run failed, and a run succeeds only when
  # every job and every matrix leg did: both Linux architectures, macOS and Windows.
  if ! gh run watch "$run" --exit-status; then
    die "a build failed (Linux, macOS or Windows, the run above says which).
       NOTHING has been published, there is no tag and no GitHub release, so there
       is nothing to clean up.

       The version bump is committed and pushed, which is harmless. Fix what broke,
       commit and push it, then run:

         dev/release.sh --resume

       which releases the same version once the build is green."
  fi
  echo "  every build passed: Linux x86_64 and aarch64, macOS, Windows"
}

if [ "$RESUME" = "0" ]; then
  step "Pushing"
  git push -q origin main
  echo "  pushed $(git rev-parse --short HEAD)"
fi

# Not on a resume. The build already passed for the commit this version was released
# from; a resume is finishing a publish of that same version, and the only thing that
# has moved since is whatever was fixed to get the publish working. Re-gating would
# mean a fresh fifteen minute build to finish uploading one package.
if [ "$RESUME" = "1" ]; then
  echo
  echo "  --resume: the build gate already passed for this release, not rebuilding"
elif [ "$SKIP_CI" = "1" ]; then
  warn "skipping the builds on CI (--skip-ci). Publishing is permanent, and this is
       the only check that compiles the code on a machine other than this one, and
       the only one that compiles it for macOS and Windows at all."
else
  step "Builds on clean machines"
  ci_gate "$(git rev-parse HEAD)"
fi

# pub.dev can take a while to make a just published version resolvable, and the next
# package in the order depends on it. Publishing straight into that gap can fail with a
# dependency that cannot be resolved, which looks like a broken constraint and is not.
#
# Two things learned the hard way releasing 0.1.1. pub.dev's own upload response says
# "it may take up-to 10 minutes", so the first version of this waited 90 seconds and
# gave up well inside the normal case. And giving up must not be fatal: the package is
# published by this point, and failing the release because a cache is slow throws away
# a step that cannot be repeated. So this waits far longer and then shrugs.
wait_visible() {
  local package="$1" waited=0 limit=600
  while [ "$waited" -lt "$limit" ]; do
    if python3 "$SUPPORT" published "$NEW" 2>/dev/null \
         | grep -q "^$package has $NEW\$"; then
      [ "$waited" -gt 0 ] && echo "  $package became resolvable after ${waited}s"
      return 0
    fi
    sleep 5
    waited=$((waited + 5))
    [ $((waited % 60)) -eq 0 ] && echo "  still waiting for pub.dev to list $package ($((waited / 60))m)"
  done
  warn "$package is published but pub.dev has not listed $NEW after $((limit / 60)) minutes.
       Carrying on. If the next package fails to resolve it, wait and run:
         dev/release.sh --resume"
  return 0
}

publish_one() {
  local package="$1"
  case " $PUBLISHED " in
    *" $package "*) echo "  $package is already on pub.dev at $NEW, skipping"; return 0 ;;
  esac
  step "Publishing $package"
  local log status=0
  log="$(mktemp)"
  set +e
  (cd "packages/$package" && "$DART" pub publish --force) 2>&1 | tee "$log"
  status=${PIPESTATUS[0]}
  set -e

  # pub.dev's API can lag its own uploads by minutes, so a package this script just
  # published can still read as missing when the run is resumed, and publishing it
  # again then fails with "already exists". That is the state we wanted, not an error.
  # Treating it as success is what makes the whole loop safe to run twice.
  if [ "$status" -ne 0 ] \
     && grep -qiF "Version $NEW of package $package already exists" "$log"; then
    rm -f "$log"
    echo "  $package is already on pub.dev at $NEW, nothing to do"
    PUBLISHED="$PUBLISHED $package"
    return 0
  fi
  rm -f "$log"

  if [ "$status" -ne 0 ]; then
    printf '\n'
    die "publishing $package failed.

       Already published in this run:${PUBLISHED:- none}

       The bump is committed and pushed, so nothing is lost. Fix whatever pub
       complained about, commit it, then run:

         dev/release.sh --resume

       which publishes only the packages still missing from pub.dev at $NEW."
  fi

  PUBLISHED="$PUBLISHED $package"
  wait_visible "$package"
}

if [ "$PUBLISH_FROM_CI" = "1" ]; then
  step "Tagging, and letting CI publish"
  git tag -a "$TAG" -m "$NEW"
  git push -q origin "$TAG"
else
  for package in $(python3 "$SUPPORT" packages); do
    publish_one "$package"
  done

  step "Tagging"
  git tag -a "$TAG" -m "$NEW"
  git push -q origin "$TAG"
fi

step "Opening the GitHub release"
notes="$(mktemp)"
python3 "$SUPPORT" notes "$NEW" > "$notes"
gh release create "$TAG" --title "$NEW" --notes-file "$notes"
rm -f "$notes"

step "Done"
echo "  $NEW is out."
for package in $(python3 "$SUPPORT" packages); do
  echo "    https://pub.dev/packages/$package/versions/$NEW"
done
echo
echo "  pub.dev takes a few minutes to render the new version."
if [ "$PUBLISH_FROM_CI" = "0" ] && [ -z "$LAST_TAG" ]; then
  echo
  bold "  First release. Now that the packages exist, automated publishing can be"
  bold "  set up so later releases never need a credential on this machine:"
  echo "  see docs/releasing.md, 'Moving the publish into CI'."
fi
