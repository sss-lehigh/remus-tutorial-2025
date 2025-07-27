---
# https://vitepress.dev/reference/default-theme-home-page
layout: home

hero:
  name: "Remus"
  text: "Making RDMA Programming Easy, Without Sacrificing Performance"
  actions:
    - theme: brand
      text: Tutorial
      link: /getting_started
    - theme: brand
      text: API Docs
      link: https://sss-lehigh.github.io/remus-tutorial-2025/doxygen/index.html
    - theme: brand
      text: Tutorial Slideshow
      link: http://www.cse.lehigh.edu
---

# Welcome

Remus is a C++ library that simplifies the task of using one-sided verbs to
create distributed data structures that work on systems with RDMA.  Above, you
will find three links:

- The "Tutorial" link will take you to an interactive tutorial on how to use
  Remus on CloudLab.  This tutorial was designed for an in-person event at SPAA
  2025.
- The slides from our SPAA 2025 tutorial are available through the "Tutorial
  Slides" link.
- The "API Docs" link will take you to automatically-generated documentation for
  Remus.  This is likely to be helpful as you develop more advanced programs
  with Remus.

If you have any questions about Remus, please raise an issue through
[GitHub](https://github.com/sss-lehigh/remus-tutorial-2025)
