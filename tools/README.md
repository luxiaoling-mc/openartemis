# tools/ — 命令行工具

两种形态,同一个目录:

- **C++ 工具**(`opfs` / `psb` / `asb` / `oasave` / `fontedge`):链接引擎库,直接复用引擎自己的
  读取器(`oa::fs`、`oa::emote`、`oa::runtime`、`oa::media`、`oa::render`、`oa::util`),
  不在工具里重写格式逻辑。桌面目标构建(`cmake --build build/default` 后位于
  `build/<preset>/tools/`;Android/wasm 目标按顶层 CMakeLists 跳过 tools/)。

  ```bash
  cmake --preset default && cmake --build --preset default
  ./build/default/tools/opfs info  /path/root.pfs
  ./build/default/tools/psb  info  /path/chara.psb
  ```

- **脚本**(`pfs.py` / `cdp_dump.js` / `tagcensus.py` / `tagaudit.py`):Python / Node 辅助,
  用于快速查看、标签面普查或无头浏览器观测。

---

## opfs — 项目包(PFS)工具(C++ twin of `pfs.py`)

```
opfs info    <archive>              版本/索引大小/条目数/总大小
opfs find    <archive> [substr]     列条目(可过滤)
opfs get     <archive> <name> [out] 取文件(省略 out:文本直接打印 / 二进制 hexdump)
opfs dump    <archive> <name> <out> 取文件(强制写盘,不截断)
opfs extract <archive> <dir>        整包解包(反斜杠分隔符转 '/',逐字节精确)
```

`extract` 是"文件夹启动"的搭档:`openartemis <dir>` 可直接启动解包后的项目(U11)。

## psb — E-mote PSB 工具

分两层:RAW 容器(任意 PSB,走 `PsbReader`)与 E-mote 语义模型(走 `EmoteFile`)。

```
psb info    <file.psb>                          头部/表/块统计 + E-mote 概要
psb tree    <file.psb> [--depth N] [--max N]    容器对象树
psb json    <file.psb> [out.json]               原始树转 JSON(逆向用)
psb motions <file.psb>                          objects/motions/nodes/frames + 变量表
psb sources <file.psb>                          图集来源(类型/尺寸/块号/图元数)
psb icons   <file.psb> [source]                 图元矩形/原点/属性
psb extract <file.psb> <outdir>                 所有图集导出 PNG + manifest.json
psb render  <file.psb> <out.png> [--size WxH] [--scale S] [--var k=v ...]
```

- `extract`:BC7/DXT5/RGBA8 图集统一解码成 RGBA8 再编 PNG;CJK/路径不安全字符在
  文件名里替换为 `_`;`manifest.json` 记录每个 source 的纹理信息与每个 icon 的
  矩形/原点(做图集裁切、对帧用)。
- `render`:tick-0 静态姿态渲染,`--var` 覆盖 E-mote 变量(如
  `--var face_talk=1 --var face_eye_open=0`),用来核对表情/口型/差分是否符合数据。
  例:

  ```bash
  psb render aya_0.psb aya_idle.png  --size 1280x720
  psb render aya_0.psb aya_talk.png  --size 1280x720 --var face_talk=1
  ```

## asb — Artemis 脚本二进制(ASB)工具

ASB 是编译后的脚本容器(标签 + 标签行)。引擎解码器把它还原成 **Artemis 文本语法**
(和 `[lyc]`/`[if]` 等一样的文本),所以本工具就是它的薄壳:

```
asb info    <file.asb>               头部 flag/条目数 + 标签直方图
asb labels  <file.asb>               标签清单(每行一个)
asb decode  <file.asb> [out.iet|-]   解码为文本(缺省 stdout)
asb extract <dir> [outdir]           递归解码目录下所有 *.asb
```

`asb extract` 默认输出到 `<dir>_iet`,保持相对目录结构、扩展名换成 `.iet` —— 配合
`opfs extract` 就能得到一份"可读的工程":脚本是文本,资源是原文件。

## oasave — 存档检查器

自动识别三种文档:编号存档(`SaveData`)、`saveg.dat`(g. 域)、`system.dat`(s. 域)。

```
oasave info  <file>                文档类型 + 计数
oasave vars  <file> [substr]       变量表(key = value)
oasave scene <file>                根 props + 层快照
oasave audio <file>                BGM/SE/VOICE 快照
oasave dir   <savedir>             扫描存档目录里每个文件
```

调试存档问题时:`oasave dir ./save` 看有哪些槽、各自停在哪一行脚本;`oasave vars
save/01.dat face_` 只看某前缀的变量。

## pfs.py — PFS 查看器(Python)

`opfs` 的只读 Python 版,适合没构建环境时快速查看:

```bash
python tools/pfs.py info  root.pfs
python tools/pfs.py list  root.pfs | head
python tools/pfs.py cat   root.pfs system/first.iet
```

## mkfixture.py — 合成样本生成器(Python)

造「结构最小但格式合法」的样本,让上面的工具在没有真实游戏包的环境(CI、新机器)
也能冒烟:

```bash
python tools/mkfixture.py psb  /tmp/mini.psb    # PSB v2:对象树 + 1 个块
python tools/mkfixture.py asb  /tmp/mini.asb    # ASB:2 标签 + 3 标签行
python tools/mkfixture.py pfs  /tmp/mini.pfs    # pf6:2 个文件
python tools/mkfixture.py save /tmp/saveg.dat   # 存档域映射
psb info /tmp/mini.psb && asb info /tmp/mini.asb && opfs info /tmp/mini.pfs
oasave vars /tmp/saveg.dat
```

格式不是猜的:脚本头注释逐条给出依据(psb_reader.cpp / asb.h / physfs_fs.cpp /
binary_stream.h),这些样本同时也是格式的活文档。编号存档(SaveData)不在此脚本
范围内 —— 它由引擎自己的存档测试覆盖。

## tagcensus.py — 剧情脚本标签普查(Python)

遍历解包工程的 `*.ast` 剧情表,按真实嵌套结构取标签行(block → `delay`/`vl1..5` 定时轨
→ `text`/`[N]`→`vo` 语音轨),输出频次/参数表/样本行:

```bash
python tools/tagcensus.py <project-root>                       # 频次表
python tools/tagcensus.py <project-root> --all-params          # 每个标签用到的参数
python tools/tagcensus.py <project-root> --samples uitrans,fg  # 样本行(文件:行号)
python tools/tagcensus.py <project-root> --keys --json out.json
```

`text={}`/`label={}` 是数据表(台词/跳转表),不参与统计;`vo={}` 语音轨与
`delay/vl*` 定时轨里的行都是真标签,照常计入。

## tagaudit.py — 引擎标签面 × 游戏实际用量对账(Python)

把引擎的原生标签面(`tag == "x"` 分支、`kConfigTags`/`kBuiltins` 数组、
`register_engine_tag`、引擎侧 `enqueue_tag`)与游戏产出面(`.ast`、`.iet`/解出的
ASB、Lua `function tags.x`、Lua `tag{}` 产出点)做全量 diff,判定:

- `used` — 游戏产出它,保留;
- `shadowed` — 两边都有:Lua 过滤器先跑(`adv/var.lua` 的 `e:setTagFilter(tags)`),
  返回真值即 Consume,引擎分支在执行路径上不可达(仍需看是否有 `e:tag{}` 入队点,
  入队路径**跳过**过滤器);
- `ENGINE-ONLY` — 游戏侧零产出,首选删除对象;
- `engine-produced` — 引擎自己入队(如 `e:createEmoteLayer` → `emotestatic`)。

```bash
asb extract <project>/system build/fpm_asb
python tools/tagaudit.py --src src/core --game <project> --asb-iet build/fpm_asb -v
python tools/tagaudit.py --src src/core --game <project> --verdict ENGINE-ONLY
```

判定口径与各工程登记清单以 `tagaudit.py` 的输出为准。

## tagmatrix.py — 多工程标签矩阵(Python)

`tagaudit.py` 回答"这个工程用不用",多个工程一起才回答"我们支持的任何工程用不用":

```bash
python tools/tagmatrix.py --src src/core \
  fpm=/g/fpm/gt slny=/g/slny/sss thyt=/g/thyt/root \
  --asb-iet fpm=build/fpm_asb --asb-iet thyt=build/thyt_asb \
  --tag-docs /path/to/tag-reference          # 上游一 tag 一文档的目录
```

每行一个引擎标签,列为各工程判定标记(`S` 脚本产出 / `L` Lua 消费 / `E` 引擎入队 /
`.` 无产出),尾部给出强产出者、弱证据(仅字符串命中)、入队者;`--tag-docs` 额外标注
该标签在上游参考里是否有文档(`NO` = 引擎自造面,如 `uitrans`)。

## pfs_subset.py — 从 PFS 只取脚本子集(Python)

审计一个工程只需要脚本/Lua/表:图片音频电影动辄数 GB。本工具列出归档索引、按扩展名
过滤后用 `opfs dump` 逐个取出(支持分卷 `root.pfs` + `root.pfs.000`):

```bash
python tools/pfs_subset.py build/windows/tools/Release/opfs /g/snly/root.pfs build/snly_src
python tools/tagcensus.py build/snly_src     # 随后即可普查/对账
```

## cdp_dump.js — 无头浏览器观测(Node)

给 wasm 目标用的:读 headless Chrome 页面里的日志面板(引擎 stdout/stderr 都在
那里),用于 CI/本机冒烟(见 `docs/PLATFORMS.md` §3)。

```bash
chrome --headless=new --remote-debugging-port=9222 \
       "http://localhost:8080/index.html?preload=1&args=--frames,180" &
node tools/cdp_dump.js 9222
```
