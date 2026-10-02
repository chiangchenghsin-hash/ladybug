# Phase 11 附录回帖 — Q-D ~ Q-G

> 对应 `gql_consult_q3_reply_verify.md` 附录（2026-10-02）。回帖 2026-10-02。
> §A/§B 事实核验：**未发现错误**（验证报告的 4 处修正本身均成立，落地口径自洽）。但 **Q-F 的前提要翻案**：不是文法天花板，是探机拼写错了。

---

## Q-F（先答，翻案）：`USE /path` 才是 ISO 拼写——"文法天花板"不存在

**结论：语法对称，你们读对了规则、拼错了语句。`USE GRAPH /path` 不是 GQL 语法；正确拼写是 `USE /path`（USE 后无 GRAPH 关键字）。**

证据链（vendored `GQL.g4`，均可复验）：

1. `useGraphClause : USE graphExpression;`（`:773-775`）——**USE 后没有 GRAPH**；`sessionSetGraphClause : PROPERTY? GRAPH graphExpression`（`:43-45`）——两边的名位**是同一个 `graphExpression`**，所以 SESSION SET 收 `/path`、USE 也收，天然对称。
2. `graphExpression → graphReference → catalogObjectParentReference → schemaReference`（`:246-251`、`:1421-1426`、`:1469-1472`），`schemaReference` 含 `absoluteDirectoryPath`（`:1387-1389`、`:1407-1409`）——**`/path` 在 USE 位文法内**。
3. 踩坑机制：`GRAPH` 在 `nonReservedWords`（`:3061` 起，GRAPH 在列）——`USE GRAPH /path` 里 `GRAPH` 被解析为 `objectNameOrBindingVariable`（一个名叫 "GRAPH" 的图），后面的 `/` 成为意外 token → "parse error at `/`"。观察正确，归因错了。
4. 翻译层 `rewriteGraphExpression` 已处理 `useGraphClause()->graphExpression()`（`gql_transformer.cpp:864-918`），与 SESSION SET 同码路——**`USE /path` 今天大概率已能跑**（roundtrip 改走的 SESSION SET 可以改回来验证）。

**最小切片**：schemapath.test 加 1 例 `USE /foo/mygraph MATCH ...` 双跑（预期直接绿）；README #22 的"USE GRAPH /path 文法天花板"条目改为"USE 正确拼写是 `USE /path`（GQL.g4:773），`USE GRAPH x` 是 Cypher 拼写、在 GQL 侧是语法错"。
**不做什么**：**不要**加 `USE GRAPH x → USE x` 宽容归一——`GRAPH` 是合法标识符（nonReservedWords），若用户真有一个叫 `graph` 的图，`USE graph` 必须继续可用；宽容层会把它吃掉。这不是"天花板"是"陷阱拼写"，文档化即可。

---

## Q-D typed 图异构列表残余：**挂账**（附将来要做时的最小推断集）

**判据（三条全不满足才做）**：① 语料无 collect、无语料网兜（已确认）；② 真实触发需要"typed 图 + 刻意跨类型构造列表 + 喂给 FOR/ORDER BY"——非自然查询形态；③ 残余已记 #17 且方向是"同化后自洽"不是随机错。**三条皆不满足 → 挂账，把工时给 Q-G。**

若将来触发判据（真实查询撞上 / TCK 长出相关场景），最小推断集已备好：
- **类型来源只认三种**：字面量（现有 `scanValueShapes`）、属性引用（翻译期查 catalog 列类型，`resolveAnyGraph` 先例 `gql_function.cpp:276-297`）、CAST 目标类型。其余一切表达式（函数返回/算术/CASE）= 判不出。
- **检查位只两个**：FOR 源、ORDER BY 键。同型放行 / 异型响亮拒 / 判不出放行记 #17。
**不做什么**：不做全表达式类型推断；不做运行时守卫（已判死刑：同化在 bind 期，检测点在信息抹除点下游）。

---

## Q-E GQLSTATUS 残余三问：**E1 不做、E2 维持无码、E3 做 harness 半（贴码可选）**

### E1 引擎透传错误文本映射 —— 不做，两个独立理由（各足以否决）

1. **文本契约脆弱**：引擎改文案即错码=挂，且这是把"最不稳定的面"（错误文案）上升为契约——方向反了。
2. **覆盖面先天残缺**（更本质）：rewrite 后的 Cypher 由引擎管线执行，**执行期异常不在扩展调用栈内**——扩展能截获的只有 bind/rewrite 阶段的同步错误。映射表盖不住执行期 ConversionException → 同一类错误"有时有码有时无码"，比全无码更伤契约可信度。

维持三档 note 档即正确归宿。**不做什么**：不做文本前缀映射；不尝试包执行期 catch（够不着）。

### E2 AS COPY OF 的 G2000 —— 维持无码，你们的分析是对的

「贴码反转」的成立条件是**钉码语义与拒绝原因一致**：25G03（只读事务）✓、42000（目录/语法）✓。G2000 语义是"拷贝类型不匹配"，层拒的是"功能未映射"——对**合法**拷贝语句贴 G2000 就是错码，违反"错码比无码更糟"。Create2 [7] 留 note 档是正确归宿。若哪天实现了 AS COPY OF，该场景自然转真契约。

### E3 22G0N/22G0P 跳过场景 —— 做 harness 模板替换（便宜），贴码可选

- **最小模板面**：纯文本替换 `$(randomLabelSet(k))` → `:__L1&__L2&...&__Lk`（确定性生成，无需真随机）；常量按实现定义 `minNodeLabels=1`、`maxNodeLabels=1`（表图节点恰一标签=实现事实，REPORT 脚注披露为 implementation-defined 参数）。同时处理 [8] 的 `a randomly generated label set of size` Given 步骤（识别后置 k）。
- **预期收益**：2 跳 → 2 跑。落 note 档（层拒多标签/空标签，any-error 满足）；若顺手在"节点类型标签集为空 / 标签数>1"两个拒点贴 `[22G0N]`/`[22G0P]`（语义与拒因一致，符合 E2 的判据），则升级为真契约 +2。
- **前置探机（必做）**：[7] 的 `randomLabelSet(0)` 渲染为零标签节点类型 `( {name STRING, ...})`——先确认层对零标签形态的行为（拒/怪），再接模板，避免引入不可测面。
**不做什么**：不做通用模板引擎（全语料仅这一种模板 2 处用，grep 实证）；不为实现多标签节点类型动本切片（挂账口径不变）。

---

## Q-G 层外建图冲突盲区：**做候选 A**，接口面 ~15 行

**理由**：盲区的失败形态不是静默错（引擎物理名冲突仍响亮），但会让**注册表与 catalog 发散**——schema 注册表认为 `/foo` 可用而引擎已有同名图，后续 DROP/改名语义出错时排查成本远高于现在的 15 行。这与"0 静默错"同向：发散本身不响，但长出来的错很难响。

**最小接口面**：抽一个 helper（放 `gql_function.cpp`，紧贴 `resolveAnyGraph:276-297` 的既有模式）：

```
bool graphNameExistsInCatalog(ClientContext&, const std::string& physicalName);
// 实现 = transaction + catalog->getGraphEntries 遍历比对（resolveAnyGraph 同款，~10 行）
```

CREATE SCHEMA 的"名字是图"校验 = 注册表查 **并** `graphNameExistsInCatalog(mangle(path))`；DROP SCHEMA 的"非空"校验同理并查。一处 helper、两个调用点。

**不做什么**：不监听/挂钩层外 DDL（无需实时性，冲突检查是点查）；不为层外图回填注册表逻辑路径（物理名比对已够，逻辑路径对层外图本就不可知）。

---

## 落地序建议

```
Q-F（1 例双跑 + 文档修订，半小时，先拿） 
→ Q-G（helper + 两调用点 + 双跑用例，半天）
→ Q-E3（harness 模板替换 + 前置探机，半天；贴码可选）
Q-D 挂账 / Q-E1 不做 / Q-E2 维持 —— 零工时，文档关闭。
```

预期：TCK 188 不降，跳 9 → 7（E3），`USE /path` 文法面补齐，层外盲区关闭。
