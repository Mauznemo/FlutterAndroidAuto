#!/usr/bin/env python3
"""The parts of a release that are too fiddly for shell: versions, pubspecs, changelogs.

Driven by dev/release.sh, which owns the flow, the safety gates and everything that
talks to git, gh or pub. This file only reads and rewrites files in the working tree,
and every subcommand is safe to run twice.

The three packages are versioned in lockstep, so there is one version number, one tag
and one GitHub release. What is not shared is the changelog: a commit lands in a
package's CHANGELOG.md only if it touched that package, because a note about the Linux
bundle layout is not news to somebody depending on the platform interface.
"""

import pathlib
import re
import subprocess
import sys

# Dependency order. Published in this order, because pub.dev will not accept a package
# whose dependencies it has never heard of.
PACKAGES = [
    "android_auto_platform_interface",
    "android_auto_linux",
    "android_auto",
]

REPO = pathlib.Path(__file__).resolve().parent.parent

# Which package a changed file belongs to, longest prefix first.
#
# tools/ maps to the Linux package on purpose: setup-dev-machine.sh, build-aasdk.sh and
# port-aasdk.sh exist to build the vendored aasdk, and install-echo-cancel.sh and
# wireless-ap.sh configure the machine it runs on. All of that is news to somebody
# depending on android_auto_linux and to nobody else.
#
# Anything not matched here, which is the root documents, dev/ and the workflows, is in
# no package's changelog. A changelog is for a person consuming the package, and how
# the repository runs its own CI is not something they consume.
PATH_OWNERS = [
    ("packages/android_auto_platform_interface/", "android_auto_platform_interface"),
    ("packages/android_auto_linux/", "android_auto_linux"),
    ("packages/android_auto/", "android_auto"),
    ("tools/", "android_auto_linux"),
]

# Only these reach a changelog. chore, docs, test and revert are about the repository
# rather than about the package's behaviour.
CHANGELOG_TYPES = {"feat": "Added", "fix": "Fixed", "refactor": "Changed"}

SEMVER = re.compile(
    r"^(?P<major>0|[1-9]\d*)\.(?P<minor>0|[1-9]\d*)\.(?P<patch>0|[1-9]\d*)"
    r"(?:-(?P<pre>[0-9A-Za-z.-]+))?$"
)


def fail(message):
    print(f"error: {message}", file=sys.stderr)
    raise SystemExit(1)


def pubspec(name):
    return REPO / "packages" / name / "pubspec.yaml"


def changelog(name):
    return REPO / "packages" / name / "CHANGELOG.md"


def read_version(name):
    text = pubspec(name).read_text()
    match = re.search(r"^version:\s*(\S+)\s*$", text, re.MULTILINE)
    if not match:
        fail(f"{pubspec(name)} has no version field")
    return match.group(1)


def parse(version):
    match = SEMVER.match(version)
    if not match:
        fail(
            f"'{version}' is not a semantic version. Expected MAJOR.MINOR.PATCH, "
            "optionally followed by a prerelease such as 1.2.0-beta.1"
        )
    return match


def sort_key(version):
    """Orders two versions the way pub does, prerelease sorting below its release."""
    match = parse(version)
    numbers = (
        int(match.group("major")),
        int(match.group("minor")),
        int(match.group("patch")),
    )
    pre = match.group("pre")
    if pre is None:
        # No prerelease sorts above any prerelease of the same numbers.
        return (numbers, 1, ())
    parts = []
    for part in pre.split("."):
        # Numeric identifiers compare numerically and below alphanumeric ones.
        parts.append((0, int(part), "") if part.isdigit() else (1, 0, part))
    return (numbers, 0, tuple(parts))


def cmd_current():
    """Prints the shared version, refusing if the three have drifted apart."""
    versions = {name: read_version(name) for name in PACKAGES}
    if len(set(versions.values())) != 1:
        listing = "\n".join(f"  {n}: {v}" for n, v in versions.items())
        fail(
            "the three packages are on different versions, which this script assumes "
            f"never happens:\n{listing}\n"
            "Put them back in step by hand before releasing."
        )
    print(next(iter(versions.values())))


def cmd_check(new):
    """Refuses a version that is not a step forward from the current one."""
    current = read_version(PACKAGES[0])
    parse(new)
    if sort_key(new) <= sort_key(current):
        fail(f"{new} is not greater than the current version {current}")
    print(new)


def cmd_bump(new):
    """Rewrites the three pubspecs: version, cross constraints, and publishability.

    The cross constraints have to move with the version. They are what makes the local
    workspace resolve android_auto's dependencies to the sibling directories instead of
    to pub.dev, and a caret constraint only does that while the local version satisfies
    it. Leave them at ^0.1.0 while bumping to 0.2.0 and the next `flutter pub get`
    quietly starts resolving against pub.dev.
    """
    parse(new)
    for name in PACKAGES:
        path = pubspec(name)
        text = path.read_text()

        text = re.sub(
            r"^version:\s*\S+\s*$", f"version: {new}", text, count=1, flags=re.MULTILINE
        )

        # path: deps cannot be published. A caret constraint can, and still resolves
        # locally inside the workspace.
        for dependency in PACKAGES:
            text = re.sub(
                rf"^(  ){dependency}:\n    path: \.\./{dependency}\s*$",
                rf"\g<1>{dependency}: ^{new}",
                text,
                flags=re.MULTILINE,
            )
            # Already a constraint from an earlier release: move it on.
            text = re.sub(
                rf"^(  ){dependency}: \^\S+\s*$",
                rf"\g<1>{dependency}: ^{new}",
                text,
                flags=re.MULTILINE,
            )

        # publish_to: none is the guard against publishing by accident, and it has done
        # its job up to here. Releasing is the deliberate act it was guarding against.
        text = re.sub(
            r"^# publish_to stays until.*?\npublish_to: none\n",
            "",
            text,
            flags=re.MULTILINE | re.DOTALL,
        )
        text = re.sub(r"^publish_to: none\n", "", text, flags=re.MULTILINE)

        path.write_text(text)

    _ensure_false_secrets()
    print(f"pubspecs rewritten to {new}")


def _ensure_false_secrets():
    """Tells pub that aasdk's head unit key is not a leak.

    pub.dev refuses a package containing an RSA private key, and aasdk carries the
    publicly known Google Automotive Link pair. It cannot simply be excluded: aasdk's
    CMake reads both files to compile them in, so a package without the key does not
    build.
    """
    path = pubspec("android_auto_linux")
    text = path.read_text()
    if "false_secrets:" in text:
        return
    path.write_text(
        text.rstrip()
        + """

# aasdk carries the publicly known Google Automotive Link head unit certificate and its
# key, the same pair every open source Android Auto implementation uses, and its CMake
# reads both to compile them in. So the key cannot be left out of the package, and
# pub.dev's leak detection has to be told that it was never a secret. See the Licence
# section of the README.
false_secrets:
  - /linux/third_party/aasdk/cert/headunit.key
"""
    )


def _commits(since):
    """Every commit since `since`, as (type, subject, files). Newest first."""
    span = f"{since}..HEAD" if since else "HEAD"
    result = subprocess.run(
        ["git", "log", span, "--pretty=format:\x1e%H\x1f%s", "--name-only"],
        cwd=REPO,
        capture_output=True,
        text=True,
        check=True,
    )
    commits = []
    for record in result.stdout.split("\x1e"):
        record = record.strip("\n")
        if not record:
            continue
        head, _, rest = record.partition("\n")
        _, _, subject = head.partition("\x1f")
        files = [line for line in rest.split("\n") if line.strip()]
        kind, _, text = subject.partition(": ")
        if kind in CHANGELOG_TYPES and text:
            commits.append((kind, text, files))
    return commits


def _owners(files):
    """Which packages a commit's files belong to."""
    found = set()
    for path in files:
        for prefix, owner in PATH_OWNERS:
            if path.startswith(prefix):
                found.add(owner)
                break
    return found


def _entries_for(name, commits):
    """The changelog body for one package, or None when it has no news."""
    grouped = {heading: [] for heading in ("Added", "Fixed", "Changed")}
    for kind, text, files in commits:
        if name in _owners(files):
            grouped[CHANGELOG_TYPES[kind]].append(text)
    if not any(grouped.values()):
        return None
    lines = []
    for heading in ("Added", "Fixed", "Changed"):
        items = grouped[heading]
        if not items:
            continue
        lines.append(f"### {heading}")
        lines.append("")
        # Oldest first reads as a narrative; git log hands them over newest first.
        lines.extend(f"- {item}" for item in reversed(items))
        lines.append("")
    return "\n".join(lines).rstrip() + "\n"


def cmd_changelog(new, since):
    """Writes a section for `new` into each package's CHANGELOG.md.

    Idempotent: a section for this version is replaced rather than added again, so the
    script can be run twice without stacking duplicates.
    """
    parse(new)
    commits = _commits(since or None)
    written = []
    for name in PACKAGES:
        body = _entries_for(name, commits)
        path = changelog(name)
        text = path.read_text() if path.exists() else ""

        if body is None:
            # Nothing touched this package, but pub warns when the changelog does not
            # mention the version being published, and lockstep means it is being
            # published regardless. Say so plainly rather than inventing entries.
            body = "No changes. Released alongside the other android_auto packages.\n"
            written.append((name, 0))
        else:
            written.append((name, body.count("\n- ")))

        section = f"## {new}\n\n{body}"
        # Drop an existing section for this exact version, up to the next heading.
        text = re.sub(
            rf"^## {re.escape(new)}\n.*?(?=^## |\Z)", "", text, flags=re.MULTILINE | re.DOTALL
        )
        path.write_text(f"{section}\n{text.lstrip()}")

    for name, count in written:
        print(f"  {name}: {count} entr{'y' if count == 1 else 'ies'}")


def cmd_verify_local():
    """Asserts the workspace still resolves the three packages to sibling directories.

    This is the check that catches the bump going wrong in the one way that has no
    symptom. The cross package dependencies are caret constraints so that pub will
    accept them, and they resolve locally only because a Dart workspace prefers its own
    members and the local version satisfies the constraint. Bump the versions without
    bumping the constraints and pub quietly starts resolving against pub.dev instead,
    at which point editing android_auto_linux stops affecting the example app and
    nothing says why.
    """
    import json

    config = json.load(open(REPO / ".dart_tool" / "package_config.json"))
    roots = {entry["name"]: entry["rootUri"] for entry in config["packages"]}
    for name in PACKAGES:
        root = roots.get(name)
        if root is None:
            fail(f"{name} is not in the resolved package config at all")
        # Only these three. android_auto_example shares the prefix and lives in
        # example/, so a prefix test here reports a problem that is not one.
        if f"packages/{name}" not in root:
            fail(
                f"{name} resolved to {root} rather than to its directory in the "
                "workspace. The cross package constraints and the versions have gone "
                "out of step."
            )
    print("  the workspace still resolves all three locally")


def cmd_published(version):
    """Asks pub.dev what exists, one line per package.

    Prints `<name> missing`, `<name> has <version>` or `<name> latest <version>`.

    pub.dev is the only authority on what has been published, and publishing is the one
    step that cannot be undone, so this must never guess. A missing package answers 404
    with an XML body rather than JSON, which is easy to mistake for a valid response;
    anything that is neither 200 nor 404 is a failure and is raised rather than treated
    as "nothing published yet", because losing this check is worst exactly when the
    network is misbehaving.
    """
    import json
    import urllib.error
    import urllib.request

    parse(version)
    for name in PACKAGES:
        request = urllib.request.Request(
            f"https://pub.dev/api/packages/{name}",
            headers={"Accept": "application/json"},
        )
        try:
            with urllib.request.urlopen(request, timeout=30) as response:
                payload = json.load(response)
        except urllib.error.HTTPError as error:
            if error.code == 404:
                print(f"{name} missing")
                continue
            fail(f"pub.dev answered {error.code} for {name}, so what is published "
                 f"cannot be established. Nothing has been changed.")
        except Exception as error:  # noqa: BLE001 - network, DNS, TLS, timeouts
            fail(f"could not reach pub.dev to check {name} ({error}). "
                 "Refusing to continue without knowing what is already published.")

        versions = {entry["version"] for entry in payload.get("versions", [])}
        if version in versions:
            print(f"{name} has {version}")
        else:
            print(f"{name} latest {payload['latest']['version']}")


def cmd_notes(new):
    """The GitHub release body: every package's section, under its own heading."""
    print(f"Released `{new}` of all three packages.\n")
    for name in PACKAGES:
        text = changelog(name).read_text()
        match = re.search(
            rf"^## {re.escape(new)}\n(.*?)(?=^## |\Z)", text, re.MULTILINE | re.DOTALL
        )
        body = match.group(1).strip() if match else ""
        if not body or body.startswith("No changes"):
            continue
        print(f"## {name}\n")
        print(body)
        print()
    print(
        "On pub.dev: "
        + ", ".join(f"[{n}](https://pub.dev/packages/{n}/versions/{new})" for n in PACKAGES)
    )


def main():
    if len(sys.argv) < 2:
        fail("no subcommand")
    command, args = sys.argv[1], sys.argv[2:]
    table = {
        "current": (cmd_current, 0),
        "check": (cmd_check, 1),
        "bump": (cmd_bump, 1),
        "changelog": (cmd_changelog, 2),
        "published": (cmd_published, 1),
        "verify-local": (cmd_verify_local, 0),
        "notes": (cmd_notes, 1),
        "packages": (lambda: print("\n".join(PACKAGES)), 0),
    }
    if command not in table:
        fail(f"unknown subcommand {command}")
    function, arity = table[command]
    if len(args) != arity:
        fail(f"{command} takes {arity} argument(s)")
    function(*args)


if __name__ == "__main__":
    main()
