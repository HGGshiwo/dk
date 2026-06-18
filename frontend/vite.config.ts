import { defineConfig } from 'vite'
import react from '@vitejs/plugin-react'
import viteCompression from 'vite-plugin-compression'

// https://vite.dev/config/
export default defineConfig({
  plugins: [
    react(),
    viteCompression({
      verbose: true,
      disable: false,
      threshold: 10240, // 体积大于 10KB 才压缩
      algorithm: 'gzip',
      ext: '.gz',
    })
  ],
  base: '/home/',
  build: {
    rollupOptions: {
      output: {
        manualChunks(id) {
          if (id.includes('node_modules')) {
            if (id.includes('antd') || id.includes('@ant-design') || id.includes('rc-')) {
              return 'antd-vendor';
            }
            if (id.includes('react/') || id.includes('react-dom') || id.includes('scheduler')) {
              return 'react-vendor';
            }
            if (id.includes('@dnd-kit')) {
              return 'dnd-vendor';
            }
            return 'vendor'; // 其他第三方库
          }
        }
      }
    }
  }
})



