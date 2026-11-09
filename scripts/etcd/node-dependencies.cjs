const { createRequire } = require("node:module");
const path = require("node:path");
// An explicit dependency directory also supports isolated validation builds.
module.exports = createRequire(
  path.resolve(
    process.env.ZLINK_ETCD_NODE_MODULES ||
      path.join(__dirname, "../../framework/languages/node/node_modules"),
    "../package.json",
  ),
);
