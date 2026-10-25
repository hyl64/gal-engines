

## 自动播放（KRKR_AUTOPLAY_MS）

`KRKR_AUTOPLAY_MS="<ms>[,x,y]"`：每 N 毫秒在给定点合成一次点击，无限循环——
KAG 游戏由此自动推进（不再每句点击）。与 `KRKR_AUTO_CLICK`（点 Start）组合：

```bash
KRKR_AUTO_CLICK="1131,446,8000" KRKR_AUTOPLAY_MS="2500,400,650" krkrrun startup.tjs
```

实测（真实 KAG 工程）：50 秒加载 35 次剧本（first.ks→0.ks→1.ks↔uiscript.ks），
零错误——自动模式全速推进整章。
