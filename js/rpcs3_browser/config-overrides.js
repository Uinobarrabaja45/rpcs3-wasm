const HtmlWebpackPlugin = require('html-webpack-plugin');

module.exports = {
  devServer: function(configFunction) {
    return function(proxy, allowedHost) {
      const config = configFunction(proxy, allowedHost);

      // COOP/COEP are required for SharedArrayBuffer (pthread-backed WASM
      // workers) which the RPCS3 PPU/SPU JIT needs at runtime.
      config.headers = {
        'Cross-Origin-Embedder-Policy': 'require-corp',
        'Cross-Origin-Opener-Policy': 'same-origin'
      }

      return config;
    };
  },
  webpack: function(config, env) {
    const webpackPlugin = config.plugins.find((plugin) => plugin instanceof HtmlWebpackPlugin);
    if(webpackPlugin) {
      webpackPlugin.userOptions.minify = false;
    }
    // Pin the heavy WASM bundles in their own chunk so they don't end up in
    // the same module graph as the em-pthread runtime (which causes a
    // circular import when both are loaded by a Worker).
    if(config.optimization && config.optimization.splitChunks) {
      config.optimization.splitChunks.cacheGroups = {
        ...config.optimization.splitChunks.cacheGroups,
        wasm: {
          test: /[\\/]src[\\/]Rpcs3(512|1024|2048|Dyna)\.(js|wasm)$/,
          name: 'wasm-module',
          chunks: 'all',
          enforce: true,
        },
      };
    }
    return config;
  }
};
