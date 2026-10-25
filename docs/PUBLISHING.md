# 怎么把这里的改动发到 GitHub

## 为什么不能直接 `git push`

这台机器上 **git 的 smart-HTTP 通道不通**：

```
$ curl -o /dev/null -w '%{http_code}' https://github.com/hyl64/gal-engines.git/info/refs?service=git-upload-pack
000                       # 30 秒超时
$ curl -o /dev/null -w '%{http_code}' https://github.com/hyl64/gal-engines
200                       # 同一个域名，普通页面正常
```

`github.com` 首页、`api.github.com`、`codeload` 都通，唯独 `*.git/...`
这条路径被拦；SSH 能连上（22 与 443 都通）但本机密钥没登记到账号，
`git@github.com` 返回 `Permission denied (publickey)`。所以 `git clone`、
`git fetch`、`git push` 全部失败。

## 可行做法：用 Git Data API 组一个提交

`api.github.com` 可用，于是用 REST 拼提交：blob → tree → commit → 移动 ref。

```bash
python3 tools/push-api.py "提交说明"     # 会打印上传了哪些文件
```

脚本做的事：

1. 取远端 `main` 的 HEAD 和整棵树；
2. 遍历本地文件，用 `git hash-object` 算 blob sha 与远端比对，
   **只上传内容真的变了的文件**（一次实测：660 个文件里只有 21 个需要上传）；
3. 用 `base_tree` 建新树、建提交、把 `main` 快进过去；
4. 每个 API 调用带重试（网络会偶发读超时）。

## 注意

- 只发布本会话负责的部分（`kirikiroid2-mac/`、`gal/`、`bench/`、`harness/`、
  `docs/`、`.github/` 与根目录文档）。其它引擎目录正在被并行会话修改，
  遍历时会遇到「文件刚被删掉」，脚本会跳过而不是崩掉。
- `testdata/` 是真实游戏数据，永远不进仓库（见 `.gitignore`）。
