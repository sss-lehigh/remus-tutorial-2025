// Normally we'd use `import { defineConfig } from 'vitepress'`, but since we
// want to use mermaid, and since mermaid doesn't play nicely with others, we
// instead use this:
import { withMermaid } from "vitepress-plugin-mermaid";

// NB:  For more details, see <https://vitepress.dev/reference/site-config>
export default withMermaid({
  title: "Remus",
  description: "Making RDMA Programming Easy, Without Sacrificing Performance",
  markdown: { math: true },
  outDir: "./dist",
  themeConfig: {
    // Use `nav` if you want navigation along the top.  See
    // <https://vitepress.dev/reference/default-theme-config> for more info
    //
    // nav: [
    //   { text: 'Home', link: '/' },
    //   { text: 'Examples', link: '/markdown-examples' }
    // ],

    // Use `sidebar` for things that show while you're *in* the docs, but not on
    // the landing page.
    sidebar: [
      {
        // NB:  Use `text` if you want a header on the sidebar
        //
        // text: 'Examples',
        items: [
          { text: 'Getting Started', link: '/getting_started' },
          { text: 'Building And Running', link: '/building_and_running' },
          { text: 'Connecting Nodes', link: '/connecting_nodes' },
          { text: 'Allocating and Accessing Memory', link: '/allocating_and_accessing_memory' },
          { text: 'Building A Concurrent Data Structure', link: '/lazy_list' },
          { text: 'Advanced Remus Features', link: '/advanced_features' },
        ]
      }
    ],

    // Use `socialLinks` if you want links along the top, e.g., for the git
    // repo.
    //
    // socialLinks: [ { icon: 'github', link:
    //   'https://github.com/vuejs/vitepress' }
    // ]
  },
  // Change the folder where the vitepress cache goes, because it doesn't need
  // version control, but it's annoying when child folders need their own
  // .gitignores.
  cacheDir: "./.cache",
})
