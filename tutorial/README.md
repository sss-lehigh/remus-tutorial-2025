# Tutorial Website

This folder holds the sources for the tutorial website.

## Getting Started

You will need `npm` and `node.js` in order to build the website.  Once you have
them installed, type `npm install` to fetch dependencies.  Then you can use the
following scripts:

- `npm start`: Run a development server with hot reloading, so you can test your
  changes to the site.
- `npm run build`: Build a deployable version of the site in the `dist` folder.
  Note that the `dist` folder is not version controlled.  You will need to
  deploy the site after running `npm run build`.

## A Crash Course in VitePress

This site uses VitePress, which is probably the easiest markdown-based site
generator within the node.js ecosystem.  There is a file called `index.md` that
will be compiled into the main landing page.  The `actions` section of the
metadata at the top of that file will appear as buttons on the landing page.
Typically, you'll want these buttons to link to other markdown files.

`.vitepress/config.mts` contains a `sidebar`, which will show any time that a
page other than the landing page is showing.  This provides a clean facility for
having a navigation menu on the left side as visitors work through the tutorial.

Other than that, The only files of interest are the various markdown files that
are referenced by `index.md` and/or `config.mts`.  In those files, you can use
standard markdown, and also some helpful plugins like code blocks, LaTeX math,
Mermaid diagrams, GitHub tables, and emojis.
