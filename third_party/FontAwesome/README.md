# Font Awesome Free — SeimiRender 导航图标子集

- **来源**：Font Awesome Free（Classic / Solid 风格）官方原始 SVG，逐字未改（保留 viewBox、路径与官方授权注释）。
- **版本**：6.7.2（固定 tag：`FortAwesome/Font-Awesome@6.7.2`）
- **下载命令**（复现）：

  ```
  curl -O https://raw.githubusercontent.com/FortAwesome/Font-Awesome/6.7.2/svgs/solid/<name>.svg
  curl -O https://raw.githubusercontent.com/FortAwesome/Font-Awesome/6.7.2/LICENSE.txt
  ```

- **收录清单**（仅本项目侧栏导航所需，不引入 Pro/Light/Thin，也不打包网页 CSS/JS）：

  | 文件 | 用途 |
  |---|---|
  | `house.svg` | 导航·概览 |
  | `sliders.svg` | 导航·设置 |
  | `arrows-rotate.svg` | 导航·更新 |
  | `file-lines.svg` | 导航·日志 |
  | `lock.svg` | 导航·登录态 |
  | `microchip.svg` | `--ui-demo` 控件陈列室页的导航项 |

- **接入方式**：`CMakeLists.txt` 的 `qt_add_resources` 将这些 SVG 编入 qrc
  （运行时路径 `qrc:/third_party/FontAwesome/<name>.svg`），由 `qml/components/FaIcon.qml`
  加载并着色。运行时不访问 CDN、不依赖源码绝对路径。
- **授权**：Icons — CC BY 4.0；Fonts — SIL OFL 1.1；Code — MIT（全文见 `LICENSE.txt`）。
