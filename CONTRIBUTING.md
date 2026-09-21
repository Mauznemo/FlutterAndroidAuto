# Contributing

Thanks for wanting to help. Bug reports, documentation fixes and code are all welcome,
and a small first pull request is a perfectly good one.

This file has two halves. The first is for anyone who has not contributed to an open
source project before: the mechanics, step by step. The second is the style for
commits and pull requests, which is short but not optional, because it is what keeps the
history readable years later.

## Before you start

- **Found a bug?** Open an issue. Say what you did, what happened, and what you expected
  instead. Versions, logs and the exact command you ran are worth more than a paragraph
  of description.
- **Small fix?** Just send the pull request. A typo, a broken link or an obvious one line
  bug does not need an issue first.
- **Bigger change, or a new feature?** Open an issue first and describe the idea. It
  costs you five minutes and can save you a weekend of work on something that does not
  fit the project's direction.
- **Do not send a pull request that reformats files you did not otherwise touch.** A
  diff of five real lines inside two thousand reformatted ones cannot be reviewed.

## New to GitHub? Start here

You do not need write access to this repository. The normal flow is: copy the project to
your own account, change it there, and ask for the change to be pulled back.

**1. Fork the repository.** Press the *Fork* button at the top right of the GitHub page.
You now have your own copy at `github.com/<you>/<project>`.

**2. Clone your fork to your machine.**

```bash
git clone https://github.com/<you>/<project>.git
```

**3. Make a branch.** Never work on `main` in your fork. One branch per change, named
after what it does.

```bash
git checkout -b fix/notification-icon
```

**4. Make the change.** Keep it to one topic. Two unrelated fixes are two branches and
two pull requests, which get reviewed and merged quickly. One branch holding both gets
stuck behind whichever half is more debatable.

**5. Check it before committing.** See [Before you open a pull request](#before-you-open-a-pull-request)
below.

**6. Commit.** Follow [Commit style](#commit-style).

```bash
git add -A
git commit -m "fix: notification icon size"
```

**7. Push the branch to your fork.**

```bash
git push -u origin fix/notification-icon
```

**8. Open the pull request.** GitHub will show a banner offering to open one from the
branch you just pushed, or you can press *Contribute* on your fork. The target is this
repository's `main` in most cases. Follow
[Pull request style](#pull-request-style).

## Commit style

One line, nothing else:

```
<type>: <subject>
```

- **Types:** `feat`, `fix`, `refactor`, `docs`, `chore`, `test`, `revert`.
- **No body.** If the change genuinely needs explaining, explain it in the pull request
  description, where people will actually read it.
- **No scope.**
- **Lowercase, no trailing period.**
- **Subject is 2 to 10 words.** Terse and informal is fine.

The rule that decides the wording:

- **`fix:` names the broken behaviour, not the repair.** What was wrong, as a reader of
  the log would recognise it.
- **`feat:` names the capability**, as the user of it would ask for it.
- Write the shortest phrase that would still identify the commit a year from now, then
  stop.

Good:

```
fix: notification icon size
feat: add FCM support
fix: session does not resume after the cable is replugged
refactor: split the author's tooling into dev/
docs: fix documentation that no longer matches the code
```

Not good, and why:

```
Fixed the bug.                            no type, capitalised, says nothing
fix: changed IconSize to 24dp             names the repair, not the behaviour
fix: notification icon was the wrong size after the theme refactor landed   too long
feat: FCM.                                trailing period
update stuff                              no
```

**Commit to the branch you are on.** Do not open a fresh branch mid change, and do not
merge `main` into your branch out of habit. If the branch has drifted far behind and the
maintainer asks, rebase.

## Pull request style

### Title

Exactly the commit rules again: `<type>: <subject>`, one line, lowercase, no trailing
period, no scope, 2 to 10 words. Someone reading the title alone should know what the
pull request is about.

A check on the pull request verifies this and explains what is wrong if it fails. Edit
the title and it runs again, there is no need to push anything.

### Body

Start with **one short paragraph**: what was broken and what you did about it, or for a
feature, what it is and why it is useful. Keep it as short as it can be and still be
true.

For a small change, that paragraph is the entire description. Stop there.

For something bigger, add sections. Every section after the opening paragraph gets an
`## H2` header, and the header is as short as possible. Use bullet points or short
paragraphs for what changed and why. If you added tests, or verified the behaviour by
hand, list that too, briefly. Add other sections when they are actually needed, such as
*Breaking changes* or *Closed issues*.

Someone reading the description should get an overview of everything important in well
under a minute. If they are reading for five, it is too long.

A small one, complete:

```markdown
The notification icon was rendered at the launcher icon's size, so it overflowed its
bounds on Android 13 and up. It now uses the density independent size the platform
expects.
```

A larger one:

```markdown
Adds push notification support through FCM, so the app can be woken by the server
instead of polling every few minutes on battery.

## What changed

- New `PushService` that registers the device token on login and clears it on logout.
- The poll loop is now the fallback, used only when registration fails.
- Token refresh is handled in the background, no user visible step.

## Breaking changes

Apps embedding this need a `google-services.json`. The build fails with a readable
message if it is missing.

## Testing

- Unit tests for token registration and refresh.
- Verified end to end on a Pixel 8 and an emulator, foreground and killed.
```

### One more rule, everywhere

**Never use em dashes or en dashes**, not in code, comments, documentation, commit
messages, pull request descriptions or user facing strings. Use a comma, parentheses, or
a separate sentence.

## Before you open a pull request

Give your own diff one honest read first. `git diff main...HEAD` shows everything you are
about to propose, and it catches the debug print, the commented out block and the
accidentally deleted line more reliably than a reviewer will.

Then make sure the project still builds and its checks still pass. For this repository:

```bash
flutter pub get && flutter analyze && flutter test packages/*/test
```

and, for anything touching the native or plugin code, build and run the example app:

```bash
cd example && flutter run -d linux
```

Match the style of the code around your change: its naming, its idiom, how much it
comments. A change that reads like the file it lives in is a change that gets merged.
Project specific rules that are not obvious from the code are in `AGENTS.md` and in
`docs/`.

## What happens next

A maintainer will read it. Expect questions, and expect to be asked for changes. That is
review working, not a rejection.

When you are asked for changes, commit them on the same branch and push again. The pull
request updates itself. Do not close it and open a new one, and do not rewrite the
history of a branch that is already under review unless you are asked to, since it makes
the review comments hard to follow.

If a pull request goes quiet, a polite comment after a week or so is entirely welcome.

## Licensing

By contributing you agree that your contribution is licensed under the same licence as
the project, see [`LICENSE`](LICENSE). This one is GPL-3.0-or-later. If you paste code
from somewhere else, say where it came from in the pull request, and check that its
licence allows it.
