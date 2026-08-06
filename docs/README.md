# The site

Published at <https://dongeronimo.github.io/Putorana/> by GitHub Pages, built
from this folder on `main`. There is no build step to run and nothing to install:
push to `main` and GitHub runs Jekyll over `docs/` itself.

This file is in `exclude` in `_config.yml`, so it is a note to whoever edits the
folder and not a page on the site.

## Adding an article

One file in `_articles`, named `some-title.md`. No date anywhere: not in the
filename, not in the front matter, not in the URL. The unit here is a problem
solved, which does not line up with a day.

```markdown
---
layout: article
title: "What Fixed the Reconstruction, and What Only Looked Like It Would"
order: 1
summary: >-
  One or two sentences. This is what the index shows under the title, so it is
  the only thing a reader has to decide from.
---

Body starts here. No `# Title` heading: the layout prints the title from the
front matter, and a second one would show up twice.
```

`order` is the next unused integer, and it is required. It is what the index
sorts on now that there is no date to sort on, and an article without it breaks
that sort rather than quietly appearing in an arbitrary place.

Sections start at `##`, because the title is the page's only `<h1>`.

## Screenshots

`assets/<same-slug-as-the-article>/`, so the folder that holds the evidence is
found from the article that rests on it. The files keep the camera's own names
(`Screenshot_20260806_153312.png`) because the articles cite them as timestamped
evidence, and renaming them would throw away the one thing that makes them
checkable.

Reference them through `site.baseurl`, never as a bare relative path. An article
is served from `/Putorana/its-title/`, so `foo.png` resolves against that URL and
404s, and `/assets/foo.png` misses the `/Putorana` prefix that a project page is
served under:

```html
<figure>
  <img src="{{ site.baseurl }}/assets/what-fixed-the-reconstruction/Screenshot_20260804_112807.png"
       alt="A fan meshed in the round">
  <figcaption>A fan meshed in the round, with the corner and wall closing around it</figcaption>
</figure>
```

`<figure>` rather than `![alt](src)` because the caption is worth showing.
Markdown has no syntax for one, and in writing where the screenshot IS the
result, "what am I looking at" belongs under the picture rather than hidden in
an alt attribute. Both are set: the caption for everyone, the alt for whoever is
not seeing the image.

`assets/2026-08-06/` predates this convention and has no article yet. It gets
renamed to the slug of whichever article claims it.

## Checking it before pushing

Optional, and needs Ruby. `gem install bundler jekyll`, a `Gemfile` with
`gem "github-pages", group: :jekyll_plugins`, then `bundle exec jekyll serve`
from this folder. Worth it for an article with a lot of markup; overkill for
prose. Otherwise the feedback loop is the Actions tab, which reports a build
failure within a minute of the push.
