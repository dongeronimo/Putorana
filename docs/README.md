# The dev diary

Published at <https://dongeronimo.github.io/Putorana/> by GitHub Pages, built
from this folder on `main`. There is no build step to run and nothing to install:
push to `main` and GitHub runs Jekyll over `docs/` itself.

This file is in `exclude` in `_config.yml`, so it is a note to whoever edits the
folder and not a page on the site.

## Adding an entry

One file in `_posts`, named `YYYY-MM-DD-some-title.md`, dated the day the work
happened rather than the day it was written up. The date in the filename is what
Jekyll orders and routes by, so it is not decoration.

```markdown
---
layout: post
title: "What Fixed the Reconstruction, and What Only Looked Like It Would"
date: 2026-08-04
---

Body starts here. No `# Title` heading: the layout prints the title from the
front matter, and a second one would show up twice.
```

Sections start at `##`, because the title is the page's only `<h1>`.

## Screenshots

`assets/YYYY-MM-DD/`, matching the entry that uses them. They are the evidence
the entries rest on, and grouping them by date is what keeps a folder of 30
files named `Screenshot_20260806_153312.png` navigable.

Reference them through `site.baseurl`, never as a bare relative path. A post is
served from `/Putorana/2026/08/04/its-title/`, so `foo.png` resolves against that
URL and 404s, and `/assets/foo.png` misses the `/Putorana` prefix that a project
page is served under:

```html
<figure>
  <img src="{{ site.baseurl }}/assets/2026-08-04/Screenshot_20260804_112807.png"
       alt="A fan meshed in the round">
  <figcaption>A fan meshed in the round, with the corner and wall closing around it</figcaption>
</figure>
```

`<figure>` rather than `![alt](src)` because the caption is worth showing.
Markdown has no syntax for one, and in a diary where the screenshot IS the
result, "what am I looking at" belongs under the picture rather than hidden in
an alt attribute. Both are set: the caption for everyone, the alt for whoever is
not seeing the image.

## Checking it before pushing

Optional, and needs Ruby. `gem install bundler jekyll`, a `Gemfile` with
`gem "github-pages", group: :jekyll_plugins`, then `bundle exec jekyll serve`
from this folder. Worth it for a post with a lot of markup; overkill for prose.
Otherwise the feedback loop is the Actions tab, which reports a build failure
within a minute of the push.
