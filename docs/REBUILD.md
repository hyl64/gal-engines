# 复现记录

时间：2026-10-25 12:20　机器：Apple M1　系统：macOS 27.0.1

```
$ ./build-all.sh
构建
  ✓ nscripter      build/onsyuri (log: /tmp/gal_build_ns.log)
  ✓ bgi            bin/bgi (log: /tmp/gal_build_bgi.log)
  ✓ kirikiri       build/krkrrun (log: /tmp/gal_build_kr.log)
  ✓ artemis        build/agame (log: /tmp/gal_build_artemis.log)
自检
  ✓ nscripter      official demo still running after 3s
  ✓ kirikiri       KAG3 displays its first line without script errors
  ✓ bgi            the trial reaches profile 1.669
  ✓ artemis        pfs, ast, runtime and 12 rendered fixture frames (not a real game)

通过 8，失败 0

$ file <每个引擎二进制>
OnscripterYuri/build/onsyuri               arm64
openbgi/openbgi-src/bin/bgi                arm64
kirikiroid2-mac/build/krkrrun              arm64
artemis/build/agame                        arm64
```
