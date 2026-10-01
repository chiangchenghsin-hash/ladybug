# Q3 咨询回帖验证报告(独立复验)

> 2026-10-02。对象:`docs/gql_consult_q3_reply.md`。方法同 Q2 轮:语料对账 + 源码对码 + 探机复跑。
> 结论先行:**回帖的事实勘误成立(我方简报事实 #3 错误,收回)**;Q3-A/B/C 的方向判断均可采纳;
> 但有 **4 处需要修正**(1 处对账错、1 处机制不全、1 处贴码脚枪、1 处口径高估),直接照抄切片会踩坑。

## 1. 事实勘误核验(回帖 §0)——成立,我方收回简报事实 #3

| 回帖断言 | 复验 | 判定 |
|---|---|---|
| 语料含 GQLSTATUS 码共 20 处 | grep 全语料逐点数:`42000`×16(Debug:13、drop1:22/34/47/62/75、Boolean1-4 各 1、Create1 schemas:38/50/63/78/112、graph-types:79)+ `25G03`(Create1:103)+ `22G0N`(graph-types:89)+ `22G0P`(graph-types:100)+ `G2000`(graphs Create2:120)= **20,完全吻合** | ✅ |
| harness 主动丢弃码断言 | `run_tck.py:516-518` 只置 `when_exception=True`,码文本不解析;`:15-17` 注释自承 | ✅ |
| 这推翻 Q3-A 前提 | 正确。"贴码=纯规范合规"不成立——语料有 20 处可执行码契约,harness 升级后可消费 | ✅ |

**我方错误与教训**:简报事实 #3「无 5 位码字样」系前轮 grep 失误(42000 是明晃晃的 5 位码,出现 16 次)。
勘误的成立使 Q3-A 从"缓做"转"做",这是回帖最有价值的一条。简报 §2 事实 #3 已加勘误注。

## 2. 对账核验(回帖 §0 末段)——一处对、一处机制不准、一处错

| 回帖断言 | 复验 | 判定 |
|---|---|---|
| Create1 9 例 + drop1 7 例 = 16 | Create1 [1]-[9]、drop1 [1]-[7] 逐格数过 | ✅ |
| drop1 [1][2] 落在 11 跳 | REPORT.md:42-43 两例 "sample data missing: data\catalogs\catalog-1.gql" | ✅ |
| 机制:"run_tck.py:427 只支持 an empty catalog" | **不准**。`:471` 有 `(.+) catalog` 名录 fixture 通道(`data/catalogs/{name}.gql`);真实原因是 **`data/catalogs/` 目录整个不存在**,catalog-1.gql 缺失。补 fixture 的修法不变 | △ |
| "catalog-1 全语料仅这 2 例用" | grep features/ 仅 drop1:7/17 | ✅ |
| 13 = Create1 8 + drop1 5 | REPORT: schemas 9 跑 8 挂([1]-[8])、drop1 5 跑 5 挂([3]-[7]) | ✅ |
| "[8]/[9] 之一应落在 parse-error 4 里" | **错**。parse-error 4 = Aggregation1 [1][2](openCypher setup)+ Aggregation3 [2](UNWIND)+ graphs Create2 [8](AS COPY OF 文法),与 schema 无关。[9] **今天已绿**(单语句切分 + 期望异常 → any-error 满足,REPORT 9 跑 1 过即它);[8] 在 19 rejected(死因是 "EXPECT OK BUT GOT ERROR",见 §3.2) | ❌ |

## 3. 逐题核验

### 3.1 Q3-A(贴码 + harness 码断言)——方向对,切片 1 有脚枪

- **三档判**(码匹配=过 / 无码有错=过-with-note / 错码=挂)设计合理,保 172 的意图正确。
- **脚枪:无差别给 `unsupported()` 贴 `[42000]` 会砸绿场景**。graphs Create2 **[7]**(钉 **G2000**)今天在绿名单:
  它期望异常,层的 `AS COPY OF` 拒绝被 any-error 口径满足(REPORT:该 feature 8 跑 4 过,挂的是 [4][5][6][8])。
  贴码 42000 + 三档判后:[7] 收到 `[42000]` ≠ 期望 `G2000` → 错码=挂 → **172 降**。
  **修正:贴码面必须收窄为"只贴语义相符处"**——schema 语义错贴 42000、READ ONLY 事务贴 25G03;
  `AS COPY OF` 拒绝**不贴码**(留 pass-with-note 档)或贴 `[G2000]`(语料钉码,与 [8]/[9] 同款"贴码反转")。
- Boolean 4×42000(展开 149 例)错误来自引擎 BinderException(无码)→ 落 pass-with-note 档,不受影响;
  graph-types [6](重复属性名,钉 42000)现绿,错误无码 → 同样安全。
- 22G0N/22G0P 在 graph-types [7][8] —— **这两例是跳过场景**(模板替换/label-set 生成,harness 跑不了),
  今天不构成可执行契约;"20 处升级为错得对"高估:实际可查 ≈ **13 处**(schema 族 12 + G2000 1),
  其余 7 处要么场景跳过(2)、要么走无码 pass-with-note(5)。

### 3.2 Q3-C(贴码反转 [8]/[9])——[9] 无需修,[8] 光贴码转不绿

- `run_tck.py:538-541`:`when_exception` 时**前导语句标 Expectation.OK、仅末句标 ERROR**;
  `split_program`(:204)把 START/COMMIT 当语句边界(:47-50 NON_QUERY_STARTERS)。
  Create1 [8] 的程序被切成 `["START TRANSACTION READ ONLY\n  CREATE SCHEMA /foo", "COMMIT"]`,
  首句期望 OK → 层拒事务包裹 → "EXPECT OK BUT GOT ERROR"(REPORT:64 行原文)。
  **故:贴对 [25G03] 后码断言通过,但首句 OK 期望仍使场景挂**——需要 harness 侧一并改
  (例外场景整体化:期望异常时全部 when 语句按 ERROR 语义处理,或对事务包裹程序不切分)。
  回帖切片只写了"贴码",缺这半步;漏掉它则 188 差 1。
- [9](`CREATE SCHEMA /foo NEXT …`)**今天已绿**(NEXT 不是切分边界,整段单句+期望异常),贴 [42000] 是把它
  从 any-error 升级为真契约,回帖"让 [9] 过"的说法不准确。
- **25G03 要分方向**:层拒**一切**事务包裹,而 25G03 语义是"只读事务"。回帖"按内层语句贴 25G03"应限定为
  **READ ONLY 包裹贴 [25G03]**;READ WRITE 包裹若也贴 25G03 就是错码(该场景语料没钉,应留无码/42000)。

### 3.3 Q3-B(守卫死刑 + 静态收窄)——全部成立

- "同化发生在引擎 bind 期,守卫函数拿到的已是同化后列表"与我方 Phase 6 引擎事实一致
  (`list_creation.cpp` 归一在 bind);检测点在信息抹除点下游=原理不可行。✅ 候选 A 死刑判得对。
- "ANY 图免疫"(JSON 文本保型)、"typed 图 ORDER BY 不 splice 走原生比较=残余自洽"——与 Phase 9/10 事实一致。✅
- 静态收窄切片(翻译期查 catalog 判列类型)可行性成立:`resolveAnyGraph`(gql_function.cpp:276-297)
  已证明翻译期可查 catalog。✅ 收益与"窄上窄"的面评估合理,作为可选小切片恰当。

### 3.4 Q3-C(目录/改名/副作用)——探机证实,设计可采纳

- **关键探机复现**:`CREATE GRAPH \`foo/bar\` ANY` → `IO exception: Cannot open file. path: ...\db.foo/bar.lbdb`;
  经 `CALL GQL` 同样命中。**回帖"零 mangling 路线不存在、必须前缀改写"的前置事实成立**(补充:双引号形式
  在 Cypher 侧直接 parse 拒;裸名带 `/` 也是 parse 拒——能进引擎的带斜杠名只有反引号形,必踩文件路径)。
- `EMPTY_RESULT_CYPHER` 先例 ✅(gql_transformer.hpp:116,CREATE/DROP GRAPH 翻译已用)。
- 目录=路径前缀推导(无目录实体)、双向映射 + `_gqlsch__` 保留前缀防御、`_gql_schemas()` 表函数校验副作用:
  设计自洽,与"够用即可"一致,可采纳。catalog-1 fixture 注意披露口径:属补充**输入数据**(数据文件缺失),
  不是改语料断言;放在 `data/catalogs/` 或 harness 侧覆盖皆可,REPORT 脚注说明。

### 3.5 收益账(回帖 ~188)——数字碰巧对,构成不对

回帖"16 schema 场景全过(2 例从跳转过)→ 188":把已绿的 [9] 算进 16(重复 +1),又漏了 Debug [4]
(schema 目录冲突的重复场景,同批可绿,+1),净抵消;精确账 = **14 现挂(Debug 1 + Create1 8 + drop1 5)
+ 2 现跳(drop1 [1][2])→ 188**,且**前提是 §3.2 的 harness 半步落地**;否则 187。
其余挂账不变(parse-error 4、AS COPY OF 2、多标签 2、qualified graph name 1、graphs Create2 其它)。

## 4. 修正后的落地序(供裁决,待批准后执行)

```
A. schema 注册表 + CREATE/DROP SCHEMA 翻译 + qualified 名改写(回帖 Q3-C 切片 1+2,设计采纳)
B. 贴码(收窄版):schema 语义错→[42000];READ ONLY 事务包裹→[25G03];其余 unsupported() 不贴码
   (AS COPY OF 留无码或 [G2000],避免砸 Create2 [7])
C. harness 同批:① 例外场景整体化(修 [8] 的 OK 期望);② catalog-1 fixture;
   ③ _gql_schemas() 副作用校验;④ 三档码断言
D. Q3-B 静态收窄:可选小切片,不阻塞
```

预期:**TCK 172 → 188**(14 挂 + 2 跳转绿),20 处码断言中 ≈13 处成为真契约,0 静默错保持;
自测加 schema 双跑 + 贴码断言用例。

## 5. 复验清单(留痕)

- grep 语料 20 处码逐点数 ✅;run_tck.py:15-17/:516-518/:538-541/:47-50/:471 对码 ✅
- Create1/drop1/graph-types/Create2/Boolean/Debug 语料逐格读 ✅;REPORT.md 全量对账 ✅
- 探机:`CREATE GRAPH \`foo/bar\` ANY` → IO exception 文件路径(引擎级与 CALL GQL 级双复现)✅
- `data/catalogs/` 目录不存在、catalog-1 仅 drop1 2 例引用 ✅;探机临时文件已清理 ✅

---

# 附:Phase 11 落地过程、结果与下轮咨询问题(2026-10-02,请外部 AI 回帖)

> 上文是回帖验证;§4 落地序已按修正版执行完毕。本附给出**关联过程**、**落地后钉死事实**与**下轮开放问题**。
> 项目硬约束不变:GQL→Cypher 翻译层、不改 GQL.g4/Cypher.g4/引擎语义、红线 0 静默错答、
> TCK 语料冻结(输入数据补全/语料自相矛盾走脚注披露)、门禁「TCK 188 不降」+自测 122 全绿。

## A. 关联过程(决策链,供对齐——每一步都可复验)

1. **Q3 简报**(62a53a9)→ 外部回帖 → **独立验证**(513e9b1,即上文):回帖事实勘误**成立**
   (我方简报事实 #3 收回:语料含 20 处 GQLSTATUS 码,harness 主动丢弃);给回帖 4 处修正——
   G2000 脚枪(无差别贴 42000 会砸 Create2 [7] 绿场景)、[8] 光贴码转不绿(harness 前导 OK 期望
   + 切分)、[9] 本已绿、20 处升级高估≈13 处。
2. **修正落地序**获批:A 注册表+翻译 → B 收窄贴码 → C harness 四件套 → D 静态收窄(可选)。
3. **2 subagent 并行**:产品侧(注册表/改名/贴码/`_gql_schemas()`/自测)∥ harness 侧(run_tck.py
   三档断言/例外整体化/catalog-1 fixture/副作用校验)——两者文件零交集。产品侧 agent 两次撞
   32k 单次输出上限(全量测试输出塞进响应),主会话接管收尾(补写 schemapath 10 例双跑)。
4. **集成中新发现**(简报/回帖/验证三轮均未见,已处置):
   - **Create1 [7] 语料自相矛盾**:When 文本漏 `IF NOT EXISTS`,但标题与 `+schemas | 0` 期望
     要求它(与 [3] 同 When 文、不同期望)。按问题语料先例(Aggregation3 values-only 同款):
     allowlist 重释 When + 脚注,语料不动。若不重释,[7] 与 [3] 不可能同时过。
   - **`USE GRAPH /path` 文法天花板**:use-graph 名位不收 `/path`(parse error at `/`),
     `SESSION SET GRAPH /path` 收(graphExpression)。roundtrip 测试改走后者。
5. **门禁**:自测 122/122(schemapath 10 新增)、TCK 188 绿(68 过+120 note)/9 挂/9 跳、
   wrong-GQLSTATUS=0;三刀本地提交 8ad735e(产品)/5280124(harness)/4cf598c(文档)。

## B. Phase 11 落地事实(已实现并双跑验收,勿重议;细节见 `_HANDOVER_GQL.md` Phase 11、README #22)

| # | 事实 | 证据 |
|---|---|---|
| 1 | SCHEMA 注册表模拟:逻辑路径集合 + 目录=非空真前缀(无目录实体)+ 逻辑↔物理双向映射;物理名 `_gqlsch__`+段拼 `__`;保留前缀响亮拒 | schemapath.test 10/10 |
| 2 | CREATE/DROP SCHEMA(IF [NOT] EXISTS)九错条件 + 语句组合拒(NEXT/多语句并存)全贴 `[42000]`;READ ONLY 事务包裹贴 `[25G03]`,其余事务包裹消息一字不改 | TCK Create1/drop1/Debug 全绿 |
| 3 | 限定名改写已通(CREATE/DROP GRAPH[TYPE]、SESSION SET GRAPH);`USE GRAPH /path` 为 grammar 面不收 | 探机 + GQLParser.h:3118 |
| 4 | 三档码断言已启用:码匹配=过 / 无码=passed-with-note / 异码=failed wrong-GQLSTATUS(首失败守卫);`when_exception` 整段单 CALL GQL | run_tck.py + REPORT |
| 5 | 战果:TCK **188 绿(68 过+120 note)/ 9 挂 / 9 跳**,wrong-GQLSTATUS=0;9 挂=语料 openCypher setup 3 + `AS COPY OF` 文法 1 + 多标签 2 + LIKE 1 + AS COPY OF 2 | REPORT.md |
| 6 | 码契约现状:20 处语料码断言中 **≈13 处为真契约**(schema 族 12 + Create2 [7] G2000 走 note);引擎透传错误(Binder/Conversion)**有意不贴码** | REPORT methodology |
| 7 | 语料例外 2 件(脚注披露):Create1 [7] allowlist 重释;`data/catalogs/catalog-1.gql` 为补全语料引用的缺失输入数据 | REPORT 脚注 |
| 8 | 问题语料先例已用两次(Aggregation3 values-only、Create1 [7] 重释)——均「语料不动、harness 例外、脚注披露」 | REPORT |

## C. 开放问题(求「推荐 + 理由 + 最小切片 + 不做什么」)

### Q-D typed 图异构列表残余:挂账,还是静态收窄?(Q3-B 遗留)

现状:静态防线覆盖字面量;运行时残余=非字面量列表在引擎 bind 期静默同化(STRING 万能汇),
守卫函数已判死刑(同化在信息抹除点上游)。ANY 图免疫(JSON 保型);typed 图 ORDER BY 不 splice
=「同化后自洽、与 GQL 原义偏离」(#17 已记)。
候选 A=翻译期查 catalog 对 typed 图 FOR 源/ORDER BY 实参做列类型静态判别(同型放行/异型拒/
判不出放行记 #17)——残余从「不可判」收窄到「判不出」,纯自测覆盖(语料无 collect,无 TCK 网兜)。
候选 B=挂账。
**问**:窄上窄的面 + 无语料网兜,值不值得做?若做,表达式类型类推断规则的最小集是什么?

### Q-E GQLSTATUS 残余三问(均涉「错码比无码更糟」的权衡)

- **E1 引擎透传错误**:今天 Binder/Conversion 等不贴码(三档落 note)。要不要做**文本前缀映射表**
  (如 ConversionException→22xxx 族)?引擎错误对象无码字段(事实,勿重议),映射只能靠文本匹配
  ——文本契约脆弱,收益=真契约档+1,风险=引擎改文案即错码(=挂)。做/不做?
- **E2 AS COPY OF 的 G2000 反转**:Create2 [7] 今天绿在 note 档(层拒 AS COPY OF 无码,语料钉
  G2000)。候选:贴 `[G2000]`(「贴码反转」同 [8]/[9],契约升真档——但 G2000 语义是「拷贝类型
  不匹配」,我们拒的是「功能不支持」,对**合法**拷贝语句贴 G2000 是错码)vs 维持无码。
- **E3 22G0N/22G0P 跳过场景**:graph-types [7][8] 钉 22G0N/22G0P 但被 harness 跳过
  (`$(randomLabelSet(...))` 模板替换 + 运行时 label-set 生成)。要不要给 harness 补模板机制
  过这 2 例(码契约 +2)?最小模板替换面怎么圈?

### Q-F `USE GRAPH /path` 文法天花板:边界成立,还是我们读错了语法?

GQL.g4 的 use-graph 从句名位实测不收 `/path`(parse error at `/`),而 CREATE GRAPH /
SESSION SET GRAPH 的名位收。同一个 catalogGraphParentAndName 系规则为何 USE 位不对称——
是我们探机拼写不对(ISO 正确拼写是什么?如 `USE GRAPH GRAPH /foo.g`、双引号限定名、
`foo.g` 点号形),还是 grammar 真不对称?若真不对称:文档边界即可,还是值得在翻译层
支持某个替代拼写(语法不动)?

### Q-G 层外建图的冲突检查盲区

经层外(纯 Cypher `CREATE GRAPH`)建的图不在注册表,CREATE SCHEMA 的「名字是图」冲突检查看不见
(引擎物理名冲突仍响亮报错,无静默错)。候选 A=冲突检查时并查引擎 catalog(翻译期可查,
`resolveAnyGraph` 先例)vs B=挂账(README #22 已记)。并查的最小接口面?

## D. 已定事项(勿重议)

- 收窄贴码策略(schema/READ ONLY 专属码、AS COPY OF 无码)、三档断言设计、mangling 方案、
  目录前缀推导、问题语料脚注口径(两例先例)均已定并落地。
- 注册表 WAL 持久化=暂缓(勿重复评估);Q6 plan cache、Q7 语料约定=纯内部不咨询。
- 语料 parse-error 4(3 例 openCypher setup + 1 例文法歧义)、多标签 2=短期不修(语料/模型边界)。
- 门禁:「TCK 188 不降」+ 自测 122 全绿。

## E. 回帖格式

1. 每条断言带证据(探机/行号);纯外部事实标来源+许可证。
2. 每问给「推荐 + 理由 + 最小落地切片 + 不做什么」;若认为某问不值得做,直接说「挂账」并给判据。
3. 若发现 §B/§A 事实有误,请先指出(可独立复验)。
4. 篇幅 ≤ 150 行。
