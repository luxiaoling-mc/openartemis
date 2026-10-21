# 官方存档（BOWX / BOWG / BOWS）与我们的兼容面

本文记录**原生 Artemis 引擎**（SDK 发行版 / 各游戏的官方 exe）写出的存档容器，
以及本引擎（OASB）怎么读它们。所有布局都是拿真实存档逐字节对齐验证的：

| 夹具 | 来源 | 容器 | 落盘 → 解压 |
|---|---|---|---|
| `artemis/test4/savedata/system.dat` | HENPRI（Rev.3139） | `BOWX` v1003 | 5753 → 22412 |
| `artemis/test4/savedata/saveg.dat` | 同上 | `BOWG` v1003 | 2695 → 11869 |
| `artemis/test13/savedata/system.dat` | 穢翼のユースティア（1003） | `BOWX` v1003 | 5774 → 22879 |
| `artemis/test13/savedata/saveg.dat` | 同上 | `BOWG` v1003 | 5163 → 57501 |
| `artemis/test13/savedata/AiyokunoEustia0001.dat` | 同上（编号存档） | `BOWS` v1003 | 58406 → 430989 |

> 老调查（`openartemisX/rust/docs/SAVE_FORMAT.md`）里那句"剩 24 / 32 字节尾巴，
> 字段语义未解"——那个尾巴就是本文第 2 节的**字段目录**。当时解出来的
> `1, 0, 2, 1, 0, 2, 26336, 26578` 正是 `count=1 / start=0 / nfields=2 /
> (id=1,off=0) / (id=2,off=26336) / dir=26578`。

## 1. 容器

```
offset 0   char[4]  magic      BOWX = system.dat，BOWG = saveg.dat，BOWS = 编号存档
offset 4   u32 LE   version    实测 1002 / 1003（1003 = 现行）
offset 8   u32 LE   raw_len    inflate 之后的字节数
offset 12  …        zlib 流    （必须在这一处正常结束，且产出 == raw_len）
```

三条校验都做（`runtime_save_official.cpp` 的 `inflate_payload`）：流必须
`Z_STREAM_END`、产出必须等于头部声明、**输入必须被吃干净**（尾部残留 =
容器被截断或拼接）。只查前两条的话，"读出一半"会伪装成成功。

## 2. 负载 = 扁平字节区 + 尾部字段目录

解压后的负载不是结构化文档，而是"字节区 + 目录"：

```
last u32              = 目录偏移 dir
dir:  u32 count
      × count: u32 object_start, u32 field_count
               × field_count: u32 field_id, u32 field_offset
```

**取一个字段的字节**：起点 = 它的 offset；终点 = **同一个 object 内**下一个
更大的 offset（没有就用 limit：根 object 是 `dir`，嵌套 object 是父字段的终点）。
目录里字段只按 offset 有序、不按 id 有序，所以必须先按 offset 排序再截断。

这是这套格式唯一容易踩空的地方：字段不是定长记录，靠"下一个字段从哪开始"
隐式定界。

## 3. 各容器的字段

| 容器 | 字段 | 内容 |
|---|---|---|
| `BOWX` | 1 | `s.*` 字符串表 |
| `BOWG` | 1 | 每脚本「已读行」集合：`u32 n`，然后 `n × (u32 len, script, u32 m, m × u32 line)` |
| `BOWG` | 2 | `g.*` 字符串表 |
| `BOWS` | 30 | 变量 bank **对象**的起点偏移；该对象的字段 2 才是变量表 |
| `BOWS` | 24 | 文本图层的命令栈（`font`/`print`/`rt`/`prohibit`/`indent`…） |
| `BOWS` | 25 | 文本图层的排版矩形（每层第一组 = 当前文字框 `x/y/w/h`） |
| `BOWS` | 26 | 图像/事件图层的命令栈（`lyc`/`lyprop`/`lyevent`/`lytween`） |
| `BOWS` | 29 | 当前 BGM：一条 `splay` |
| `BOWS` | 11 | 当前消息图层 id（裸字符串） |
| `BOWS` | 4 | 场景脚本栈（`u32 n` + n 个帧，内层在前） |
| `BOWS` | 3 | 系统脚本帧（与字段 4 的单帧同形） |
| `BOWS` | 56 | `setonpush` 注册表（`key → 指令`；UI 装脚本时会重新注册，不搬） |
| `BOWS` | 28 … | 其它序列化器（实测 BOWS 根 object 有 75~76 个字段） |

### 3.1 指令（tag）编码

命令栈里的每一条、以及脚本帧里的待执行/当前指令，都是同一个形状：

```
i32 line            ← 栈里恒为 -1；帧里的"当前指令"是 0/11/71/146 这类小整数
u32 name_len + name
u32 n_attrs
× n_attrs: (u32 len, key), (u32 len, value)
```

属性名可能是位置参数（`0`/`1`/`2`，对应脚本里的 `[Back 0="3026" 1="NORM"]`），
也可能是 `file`/`id`/`function`/`data` 这类具名参数——两种都原样进 map。

### 3.2 脚本帧（字段 4 / 字段 3）

```
u32 count
× count:  u32 index
          u32 return_line
          u32 script_len + script
          u32 line
          u32 n_pending
          × n_pending: 待执行指令
          1 条"当前指令"        ← 存档瞬间正在执行的那条，任何情况下都写
```

三件事值得记住：

1. **帧是自定界的**：指令自带长度，所以不需要猜"下一帧从哪开始"。4 份真档
   解析后都**恰好**吃到字段末尾（`frames_truncated == false`）。
2. **`return_line` 属于本帧**：它是"本帧结束后回到上层脚本的哪一行"，所以调用
   栈要成对取 `(frames[i].return_line, frames[i+1].script)`（旧实现拿
   `frames[i+1].line` 当返回行，方向错了）；`return_line == 0` 表示"没有调用者
   行"（test4 的单帧、test13 的根帧都是 0），不入栈。
3. **`line` 不一定是当前指令的行号**：引擎常把位置记在块首/标签上。实测
   test13 的场景帧是 `aiy00010.asb:1332`（`[Back]` 所在行），菜单帧是
   `system/ui.asb:78`（`*ondlg` 标签行）而当前指令是下一行的 `[trans]`；
   test4 的系统帧是 `script.asb:40`（`*click` 标签行）而当前指令是
   `calllua clickEnd`。两者都是**可用的续跑点**，所以照原样用。

### 3.3 文本排版矩形（字段 25）

```
u32 count
× count:  u32 id_len + layer_id
          u32 n_groups
          × n_groups: u32 type(=2), 4 × f32 (left, top, width, height), 排版尾巴
```

尾巴是字体面/字号/字距/颜色/行高等（两代版本长度都不同：test4 是双字面 +
度量，test13 多了 ruby 面），所以**只解每层第一组的矩形**，尾巴不碰——用图层
id 重新定位下一层，而不是按字节长度跳。

矩形是画布坐标，实测都对得上 `system.ini` 的分辨率：

| 工程 | 画布 | message 层框 | name 层框 |
|---|---|---|---|
| test4（HENPRI） | 1600 × 900 | `(408, 685, 800, 190)` | `(408, 636, 600, 50)` |
| test13（穢翼） | 1280 × 720 | `(0, 0, 720, 200)` | 同层局部坐标 |

这一项不能省：test4 的 `[font]` 指令**只带 `color`**，框全在字段 25 里；不给
这个框，读档恢复出来的第一句会落到默认位置（左上角）。

字符串表编码（三个容器通用）：

```
u32 count
× count: (u32 len, bytes) key, (u32 len, bytes) value
```

键/值都是**原始字节**：键可能是 CP932；值官方一律存文本（数值也是 `"80"`），
而把 Pluto 持久化的 Lua 表（test4 的 `g.script`/`g.system`/`g.config`、
test13 的 `lua.scr`/`lua.log`）原样存成二进制块。所以解码器只切字节、不解释
语义，解释在导入层做（第 4 节）。

## 4. 导入规则（sysload）

`GameRuntime::RuntimeState::sysload()` 先看魔数：`BOW*` 走官方导入器，其余仍走
OASB。落地规则：

1. **键带域前缀**（官方本来就写全，`s.bgmvol` / `g.script`）；缺前缀时按容器
   种类补（BOWX → `s.`，BOWG/BOWS → `g.`）。
2. **`s.datapath` / `s.savepath` 一律跳过**。它们是写档那台机器的绝对路径
   （`C:\Users\koichi\…`、`C:\artemis\testgame\test13`），照抄进来会把本机的
   存档/资源路径指到别处。日志里报跳过条数。
3. **值类型推断**（`infer_official_value`）：能整串解析成整数的给 `Int`，带
   小数点/指数的给 `Float`，其余保持 `String`（字节原样）。
   必要性：官方把一切都存成文本，而 `Value::to_bool()` 对非空字符串恒为真——
   不推断的话 `[if s.status.automode]` 会把 `"0"` 读成真。
   两个例外刻意保持字符串：前导零写法（`"001"`，更像编号）与显式 `+` 号。
4. **Pluto 表在导入时转换**（`looks_like_official_pluto` +
   `convert_official_pluto`）：`g.config` / `g.script` / `g.system`、
   `lua.scr` 这些值是官方 **native Pluto** 的二进制（`u32 first/ref/type` +
   原生 `size_t` 长度），和本引擎 `pluto.persist` 的标签流不通用。原样搬过去
   的后果很隐蔽：本引擎的 `pluto.unpersist` 契约是"读不出来给空表"，于是
   游戏拿到 `{}` ——**test4 的症状就是它**：`conf.language` 丢失 →
   `get_language()` 回落到 `init.game_language`（`"ja"`）→ `lang_init()`
   认为这是首次启动，每次进游戏都弹语言选择。现在导入这一步就把官方
   Pluto 转成本引擎文档，`conf` 原样回来（`language='cn'`、`ui_lang='cn'`…），
   `lang_init` 直接 `jump … lang_init_end`。转换不动/不是表时保留原始字节，
   日志报 `N Pluto table(s) converted`。
   顺带：`pluto.unpersist` 失败不再完全静默——二进制载荷读不出来会打一条
   `[lua] pluto.unpersist: …`，纯文本垃圾则不刷屏。
5. **已读行只报数不落地**：引擎目前没有"已读状态"存储（`skip` 不做已读判定），
   所以 `BOWG` 的已读行表只用于日志（test13 有 15 个脚本 / 14252 行）。
   将来接 skip-read 时，`OfficialSave::read_lines` 就是入口。
6. **覆盖前保住原件**：`syssave` 如果发现目标位置现在放的是 `BOW*` 容器（玩家
   从官方引擎搬来的档），第一次覆盖前把原字节另存为 `<file>.official.bak`。
   官方引擎还能读回那一份，我们也不会"读进来就把它抹掉"。

## 5. BOWS（编号存档）：读档已经能用了

官方**不是**保存"脚本 + 行号"，而是保存**每个图层的命令栈**（加载时逐条重放）
+ 一个脚本栈。我们把这两样翻成本引擎的快照模型（`SaveData`），所以官方
save / qsave 现在能被 `[load]` 读进来。

### 字段 → `SaveData` 映射

| BOWS 字段 | 内容 | 落到 |
|---|---|---|
| 30 → bank → field 2 | 变量表（含 Pluto 表块） | `local_variables`（Pluto 表在导入时转成本引擎 pluto 文档） |
| 26 | 图像图层命令栈：`layer_id → {lyc, lyprop, lyevent…}` | `layers`：`lyc`/`lyprop` 的 attrs 合并成 props，`lyevent` 变成 handler row |
| 24 | 文本图层命令栈：`font`, `print`, `rt`, `/font` | `text_layers`：`print` 的 `data` 拼成文本页，`font` 属性进字体表 |
| 25 | 文本图层排版矩形 | `text_layers[].positioned/left/top/width/height` |
| 29 | 当前 BGM（单条 `splay`：file/gain/loop/pan/time） | `audio.bgm` |
| 11 | 当前消息图层 id | 决定哪个文本层是 active |
| 4 | 场景脚本栈（内层在前，含待执行/当前指令） | `current_script`/`current_line` + `call_stack` |
| 3 | 系统脚本帧 | 字段 4 缺失时的位置兜底 |

### 实测（用户用官方 exe 做的档）

| 文件 | 位置 | 变量 | 图层 | 文本层 / 框 | 帧 | BGM |
|---|---|---|---|---|---|---|
| `test4/savedata/save0001.dat` | `system/script.asb:40` | 5（4 个 Pluto 表） | 197 / 205 handler | 2 / 2 | 1 | `:bgm/hprison_bgm12a_a.ogg` |
| `test4/savedata/save0102.dat`（qsave） | 同上 | 5 | 197 | 2 / 2 | 1 | 同 |
| `test13/savedata/AiyokunoEustia0001.dat` | `scenario/main/aiy00010.asb:1332` | 13（3 个 Pluto 表） | 193 / 943 handler | 2 / 2 | 2 | 该档没有字段 29 |
| `test13/savedata_bak/AiyokunoEustia0001.dat` | `system/ui.asb:78` | 13 | 221 / 988 handler | 6 / 6 | 2 | 该档没有字段 29 |

四份都是 `frames_truncated == false`（帧一帧不差地吃到字段末尾），并各自解出
"当前指令"（`calllua` / `rp2` / `trans`）。

### 读档后画面 = 存档瞬间的画面（菜单也一起回来）

玩家是在**存档菜单里**按下存档的，所以快照里带着整个菜单（test4 是 10 个
`NO DATA` 槽位 + Q-SAVE/LOAD/VOICE/BACK，test13 是 データセーブ 网格）。读档
后菜单会留在画面上，这不是"读错了"——这就是存档瞬间的样子：

- 场景图层（f26）与文本页/文字框（f24/25）都是逐层还原的；
- 菜单按钮的点击事件（`lyevent` → handler row，含 `function`/`lua`/`key` 等
  回调参数）也一并还原，所以用游戏自己的「BACK / ゲームに戻る」就能退出菜单；
- 官方引擎读档时还有第二个解释器（UI 脚本帧）在并行跑，会把菜单收掉；我们的
  解释器只有一个线程，只恢复场景帧 ⇒ 菜单得由玩家自己退出。

这是"快照式还原"与"官方并行解释器"的差别，不是字段没读到。

真实跑通（`OA_LOAD_SLOT` 钩子，等价于游戏里点读档）：

```
[runtime] loaded savedata/save0001.dat (script system/script.asb:40;
          cleared 32 ephemeral slot(s), restored 197 layer(s), ...)
```

### 已知缺口（不假装支持）

1. **时序状态不重放**：`[lytween]`/`[wait]`/`[trans]` 这类"正在动"的东西没有
   对应字段（`lytween` 只留在字段 26 的命令栈里，我们不按 `from`→`to` 补间），
   恢复的是"当前画面 + 文本 + 变量 + 位置"。被补间的图层取 `lyprop` 里的值
   （= 存档瞬间的值），不会停在补间起点以外的地方。
2. **`lyevent` 只映射了 type/mode/function/file/label/call**，带参数的事件
   （`param.*`、`c`/`com`/`lua`/`key` 这类键）暂时丢——它们在图层被重建时由
   脚本重新注册。
3. **帧里的待执行指令只用于诊断**（日志 `at-tag=…+N pending`）：续跑点是帧的
   `line`，那条指令会被重放，它排下的待执行指令随之重建。
4. **官方写回仍不支持**（按需求不做）：覆盖槽位时写的是 OASB（存档位是玩家
   主动选的，不做自动备份）。
5. **`setonpush` 注册表（字段 56）/ 每层文本禁则（字段 66/67/75）不搬**：
   UI 脚本与文本引擎会自己重建。

### 配套改动：解释器的每帧 tag 预算

读官方档暴露了一个**与格式无关**的引擎缺口：HENPRI 的 `system/script.asb`
(以及不少 Artemis 游戏) 的剧情驱动是一个**轮询型主循环**：

```
*main
[calllua function="scriptMainloop"] ×11（与 scriptMainAdd 交替）
[jump label="main"]
```

整个循环里没有任何 `[wait]` —— 官方引擎靠"**每帧只推进有限条 tag**"把控制权
交回宿主，输入/时间/渲染在帧与帧之间发生。我们的 `Interpreter::run()` 原来是
`for(;;)` 跑到底，于是读档恢复到这种主循环里会在**单帧内**永久自旋（tick 不
返回、帧计数冻结，表现就是"读档成功然后卡死"）。

现在 `run()` 带每帧预算（默认 4096 条，`OA_RUN_BUDGET` 可覆盖），用完返回
`ExecutionResult::BudgetExhausted`：不清场、不装 wait，下一帧从当前位置继续。
实测同一份官方档：120 帧跑完，`layers=241`、`msg_lines=2`、`bgm=1`、
`wait=none`。

### 调试入口

```
OA_LOAD_SLOT=save0001.dat ./openartemis_test --headless --frames 60 <root.pfs>
#   启动后立刻走一次真正的 load_game（位置预检/清场/快照恢复/音频重放全在），
#   只是省掉点菜单；结果打 [app] OA_LOAD_SLOT=… → loaded/missing/corrupt/…
```

顺带记两个已定论的观察：
- test13 的字段 4 第二帧不是"标签帧"，而是 **UI 脚本帧**（`system/ui.asb:2`）；
  `rp2` 是**第一帧的当前指令**（同一脚本内的标签 tag，行号记 0）。帧结构见
  3.2 节。
- test4 的字段 4 只有一帧（`system/script.asb`）：HENPRI 的剧情驱动整个在 Lua
  里（`scriptMainloop`），剧情位置存在变量 bank 的 Pluto 表里（`g.script`），
  所以读档后是 `scriptMainloop` 把进度接上的，不靠脚本帧。

## 6. 怎么验证

```bash
# 合成夹具（自造 zlib 容器 + 全部坏容器拒绝路径 + 端到端 sysload/syssave）
./build/windows/tests/Release/official_save_test.exe

# 加跑真实官方档
OA_TEST_OFFICIAL_SAVE_DIR="…/test4/savedata"  ./official_save_test.exe
OA_TEST_OFFICIAL_SAVE_DIR="…/test13/savedata" ./official_save_test.exe

# 端到端：把官方 savedata 放进引擎的存档根，看 sysload 走哪条分支
OA_SAVE_ROOT=/tmp/oaroot ./openartemis_test.exe --headless --frames 120 <root.pfs>
#   [runtime] sysload savedata/saveg.dat: official BOWG v1003 -> 3 variable(s), 0 host-path key(s) skipped, 3 Pluto table(s) converted
#   [runtime] sysload savedata/system.dat: official BOWX v1003 -> 42 variable(s), 2 host-path key(s) skipped, 0 Pluto table(s) converted
#   [tag] jump file=system/first.iet label=lang_init_end     <- 语言已选过，不再进选择界面

# 读官方编号存档（等价于游戏里点读档，只是省掉点菜单）
OA_SAVE_ROOT=/tmp/oaroot OA_LOAD_SLOT=save0001.dat ./openartemis_test.exe --headless --frames 60 <root.pfs>
#   [save] official BOWS v1003 → SaveData: …
#   [runtime] loaded savedata/save0001.dat (script system/script.asb:40; … restored 197 layer(s))

# 就地看官方档（含 BOWS 的字段目录 / 变量类型推断 / 已读行范围 / 命令栈映射）
oasave info <file>          # kind / version / 变量数 / object+字段目录
oasave bow  <file>          # 同上 + 已读行集合；BOWS 再展开位置/图层/文本/BGM
oasave vars <file> [substr] # 变量（非可打印字节转义，标 int/float/str）
oasave dir  <savedir>       # 整个目录逐文件认格式（官方档不再显示 unrecognized）
```

合成夹具覆盖：三种容器的字段取法、值推断（`"80"`/`"001"`/`"1.5"`/`"0"` 语义）、
host-path 规则、导入前缀兜底、sysload 落地与 syssave 备份、BOWS 槽位 →
`SaveData` 的逐项映射（位置/调用栈用 `return_line`/变量/Pluto 转换/图层 props/
lyevent/文本页/字段 25 文字框/BGM），以及坏容器路径（坏魔数/版本越界/声明长度
不符/zlib 截断/尾部垃圾/目录越界/重复字段 id/字符串表尾部多余/空键/长度越界/
缺根 object/缺字段/BOWS 缺 bank）。

真实夹具额外断言（`OA_TEST_OFFICIAL_SAVE_DIR`）：帧必须解到 `frames_truncated ==
false`、当前指令名非空、系统帧存在、当前消息图层必须有文字框、`SaveData` 里
至少一个 `positioned` 文本层。

## 7. 与 OASB 的关系

两套格式**互不兼容**，且不打算做双向：官方引擎读不了我们写出的 `OASB`，
我们写回 `OASB` 时只保证"官方原件已备份"。当前兼容面：

- **只读导入** `system.dat` / `saveg.dat`：设置 + 全局变量 + 已读行统计；
- **只读导入** 编号存档（BOWS）：位置 + 完整调用栈（`return_line`）+ 变量 +
  场景图层 + 文本页与文字框 + 当前 BGM，即"能接着玩"的集合（缺 `[lytween]`/
  `[wait]` 这类在飞状态，见第 5 节）。
