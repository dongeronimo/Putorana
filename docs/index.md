---
layout: default
---

Putorana reconstructs the real world into a virtual one: ARCore supplies tracking,
the camera image and a depth map, a TSDF fuses those depth maps into a signed
distance field, marching cubes turns the field into meshes, and Vulkan draws them
over the live camera feed. It is written in C++ for Android and runs on the CPU.

It is named after the [Putorana Plateau](https://en.wikipedia.org/wiki/Putorana_Plateau),
the main flood basalt of the Siberian Traps, because Vulkan projects get volcano
names here.

These are working notes, one article per problem rather than one per day. They
keep the wrong turns in, because the wrong turns are the expensive part to
rediscover. The code and the notes on how it all fits together live in
[the repository](https://github.com/dongeronimo/Putorana).

<h2>Articles</h2>

<p>In the order they were written. Each one assumes what came before it.</p>

{%- comment -%}
  Sorted on the explicit `order` field, not on a date, because there is no date to
  sort on. An article without that field breaks this loop rather than silently
  landing somewhere arbitrary, which is the intended failure: the sequence is the
  only thing standing in for chronology.
{%- endcomment -%}
{%- assign articles = site.articles | sort: "order" -%}
<ul class="article-list">
  {%- for article in articles -%}
  <li>
    <a class="article-list-title" href="{{ article.url | relative_url }}">{{ article.title | escape }}</a>
    <p class="article-list-summary">{{ article.summary | default: article.excerpt | strip_html | normalize_whitespace }}</p>
  </li>
  {%- endfor -%}
</ul>
