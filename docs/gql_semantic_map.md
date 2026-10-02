# GQL ↔ Cypher 语义地图(硬事实权威表)

> 定位:**可执行规格的索引**,不是散文。每行 = 一条探机/测试钉死的硬事实,并指向钉住它的
> `.test` 用例名——地图若被违反,测试就会挂。事实来源:探机复现 + 测试双跑 +
> `extension/gql/README.md` 差异清单(#n)+ `_HANDOVER_GQL.md` 交付记录。
> **规矩:今后任何新钉的语义事实,同一提交里必须带测试并回写本表,否则地图退化成传闻。**
> 方法论(怎么验证、怎么处置问题)见 `docs/gql_methodology.md`;本表只放"是什么"。
> 数据截至 2026-10-02(多跳 QPPI 轮收工),自测 161/161。

缩写:ANY = 开放 ANY 图(动态属性=JSON 列);表图 = typed/tabled 图(标签=表名)。
测试文件在 `extension/gql/test/test_files/`;TCK 钉记 `run_tck` 场景名。

---

## 一、同形反语义(同一拼写,两个世界含义不同)

| 事实 | 证据(可执行引用) | 影响面 |
|---|---|---|
| `:A:B` 冒号链在 Ladybug **ANY 图=AND、表图=OR(表并集)**——同拼写反语义;且输入侧 `:A:B` 本身就是 **GQL 语法错**(ISO 合取只有 `&`,filler 单标签槽)。三方分叉在目标方言侧(Cypher25 读 `:` 为 AND) | `labels.test` ColonLabelChainNotGqlSyntax / LabelExpressionAnyGraphParity / LabelExpressionTypedGraphParity;README #13 | 复合标签**绝不能**发冒号链进 match/谓词位;翻译层只从 `&\|!` 渲染;唯一发出的冒号链是 ANY 图 INSERT 创建位(设双标签=事实非谓词) |
| `*` 量词起跳不同:GQL `*`=**0**..∞、`+`=1..、`?`={0,1};Cypher 裸 `*`=**1** 起 | `path.test` QuantifiedEdgeStarParity / QuantifiedEdgeQuestionedParity / QuantifiedEdgePlusParity;README G035 | 单跳 QPPI → var-length 必须显式区间(`[e*0..]`),照抄 Cypher 会丢 0 次行走 |
| `USE` 拼写陷阱:`USE GRAPH x` 的 `GRAPH` 是 nonReserved,**会被当图名**;GQL 的 USE 是**查询前缀从句**(`USE /foo/g MATCH …`),无 GRAPH 关键字;独立 `USE x` 不是 GQL 语句 | `schemapath.test` 系(USE 前缀从句双跑);README #22 | 勿写 `USE GRAPH x`(不加宽容改写——会吃掉合法图名);会话拼写是 `SESSION SET GRAPH` |
| 未别名结果列名:GQL 惯例取**源文本**(`RETURN max(x)` → 列 `max(x)`);裸引擎会大写函数名 | README #16(全出口 `AS \`源文本\`` 改写) | 列名契约对齐 GQL,不是引擎习惯 |
| 布尔运算两边**都**做类型检查(`123 AND true` → BinderException)——曾被误报"Ladybug 不校验",实为 harness 正则 `.+` 不跨行的 bug | README #19;Phase 5→6 翻案记录 | 归因必须过执行验证(方法学 6) |
| `FILTER` → `WITH * WHERE`(裸 WHERE 只能挂 MATCH/WITH);结果语义不变 | README #18;`select.test`/`groupby.test` 面 | 合成面差异,无语义差 |
| `PATH_LENGTH`/`LENGTH`:GQL **无 `LENGTH()`**;`PATH_LENGTH` 映射 `LENGTH` 只吃**路径值**;列表(量词绑定)用 `size(e)` | `multihop.test` VarLenEdgeBindingWrapParity / `qpibind.test` QpibindDirectSlotSizeParity;README #8 | 两种绑定形状(路径值 vs 列表)用不同长度函数,混用响亮报错 |

## 二、路径语义精度差(引擎原语 ≠ GQL 语义,翻译层补齐)

| 事实 | 证据(可执行引用) | 影响面 |
|---|---|---|
| 引擎 `*ACYCLIC` **只保中间点两两互异**(端点可撞中间点),≠ GQL ACYCLIC(全节点互异含首尾);探机 m0→m1→m0→m2 裸 `*ACYCLIC` **放行** | `smallmodes.test` SimpleClosedTriangleParity / SimpleEndpointRepeatExcludedParity;README #7 | 所有 ACYCLIC 模式必须 `*ACYCLIC` 预过滤 + `IS_ACYCLIC(p)` 全路径谓词;照抄引擎=静默错答 |
| GQL **SIMPLE** = 节点不重复、仅首尾可重合(闭合环允许,端点重复不允许) | `smallmodes.test` 同上两例红线钉 m0→m1→m0→m2=0 行、闭合三角=1 行 | 翻译 `_gql_is_simple(ANY)→BOOL`(RECURSIVE_REL→nodeIDs 判互异)+预过滤;「引擎 *ACYCLIC=ISO SIMPLE」是**错映射**(Q5-2① 证伪) |
| GQL **TRAIL** = 全边互异 = 引擎 `*TRAIL`(单 var-length 槽可直映);多跳 TRAIL 走路径变量 + `IS_TRAIL(p)` | `path.test` TrailModeParity / MultiHopTrailParity;README #7 | 多跳 TRAIL 绝不能只靠槽上 `*TRAIL` |
| 路径变量值 nodes/rels **首尾各一次**;`IS_TRAIL`=全 rel 互异、`IS_ACYCLIC`=全节点互异(自环 False)=GQL 精确语义 | `path.test` WalkModeParity / MultiHopDefaultParity;P7 交付记录 | 路径值构造与谓词语义对齐 ISO,不是近似 |
| 多跳 QPPI **有界展开**:`{n}`/`{m,m}` 内腔就地展开(单句无限制);`{m,n}`(1≤n-m≤4, m≥1)整句按计数重译 + `UNION ALL`(DISTINCT→裸 `UNION`=全局去重);共同 `USE GRAPH` 前缀剥出置顶 | `multihop.test` MultiHopFixedCountParity / MultiHopRangeUnionParity / MultiHopRangeUseDistinctUnionParity;README #23 | 区间形受限:聚合/ORDER BY/SKIP-LIMIT/OPTIONAL/写入响亮拒(union 无包装从句=grammar 天花板,MultiHopRange{Order,Aggregate,Optional,DataModifying}Rejected 钉) |
| 量词元素变量 = ISO **列表**(每重复一条目;接缝节点=相邻条目共享);外接缝**外层用户名胜**;接缝标签/属性同或一侧空才合并,冲突拒 | `qpibind.test` QpibindRangeSeamParity / QpibindOuterSeamNameWin / QpibindJunctionConflictRejected | `RETURN a` 看外层 `a`,`x` 的首条目**就是** `a`;绑定形状两种拼写(paren 内腔/直接槽)必须一致——曾一个包列表一个不包(拼写分裂) |
| 量词绑定**没有属性**(是列表):`x.prop` 响亮拒 | `unsupported.test` UnsupportedQuantifiedBindingPropertyAccess | 列表属性访问永不静默 |
| **星号合成名泄漏** = 列集静默错的固定出口:`_gql_nlN`(匿名复合标签)/`_gql_ppN`(路径 wrap)/`_gql_veN`/`_gql_ueN`/`_gql_vnN`(QPPI)都会从 `RETURN *`/`SELECT *` 漏成未声明列 | `multihop.test` MultiHopStarProjectionRejected;`unsupported.test` UnsupportedStarProjectionWithGeneratedBindings / UnsupportedStarProjectionWithPathModeWrap | 新合成名必查星号投影;当前统一响亮拒 `star projection with generated pattern bindings` |
| 无界多跳(`*`/`+`/`{m,}`)、下界 0 多跳(`?`/`{0,n}`——0 次=接缝节点认同,需节点等值,有意回避)、节点量词 `(n){q}`、嵌套量词、n-m>4、分支组合>8 = 全响亮拒(无界=表达力天花板**永久拒**) | `multihop.test` MultiHopLowerBoundZeroRejected / MultiHopRangeTooWideRejected / MultiHopBranchExplosionRejected;`unsupported.test` UnsupportedQuantifiedMultiHop / UnsupportedQuantifiedNodePattern | 面扩大只能等触发(挂账见 `_HANDOVER_GQL.md` 五) |
| 路径模式精确:TRAIL 多跳与**所有** ACYCLIC 绑路径变量加全路径谓词(引擎单用会漏闭合行走 1→2→1) | `path.test` MultiHopTrailParity / MultiHopAcyclicParity | 见 README #7/#23 |
| SHORTEST/ANY k PATHS 等 search prefix:单跳 ANY/ALL SHORTEST 直映;counted `SHORTEST k`/`GROUP(S)`、多跳上的 mode/search prefix 响亮拒 | `path.test` AnyShortestParity / AllShortestParity;`unsupported.test` UnsupportedCountedShortestSearch / UnsupportedAnyKPathsSearch / UnsupportedSearchPrefixOnMultiHop | — |

## 三、值模型断层(GQL 值世界 vs 引擎同构化/文本序)

| 事实 | 证据(可执行引用) | 影响面 |
|---|---|---|
| 引擎列表字面量**bind 期同构化**(STRING 是万能汇)会静默消型;GQL 保元素类型 | `orderability.test` MixedNumericMaxMinParity;README #17 | 三态处理:FOR 源逐元素包 `_gql_to_json`(保型);其余位置静态字面量类不一致→拒 `heterogeneous list literal`;typed 图含≥1 非字面量元素→改写 `_gql_list_checked` bind 期拒(`listguard.test` ListGuardTypedRejects) |
| 异构列表残余边界**全非静默**:聚合实参内列表不包(rewriter 止步于聚合边界)、ANY 图免检(走 JSON 路线)、ORDER BY/SET 属性值走 sourceText | README #17;`listguard.test` AnyGraphListUnwrapped | 响亮或结构性,无静默错(已覆盖形状上) |
| map/record 值(`{}`/`{k:v}`)在表达式中=引擎无对应类型 | `unsupported.test` UnsupportedMapValue;README #17 | 静态拒 `map value`(不让它死在 Cypher parse 口径混乱) |
| ANY 图 JSON 属性**文本序**比较是静默错:`>=100` 命中 33/9/33.5(文本序)、`33=33.0` False;ORDER BY 出 10,100,33,33.5,9 | `comparebridge.test` OrderOpsSentinelParity(哨兵 9 vs 100);README #21 | 比较桥 `_gql_lt/le/gt/ge/eq/ne` + ORDER BY 包 `_gql_sortkey`(Phase 10);只在 `labelGraphIsAny==true` 开 splice |
| 数字比较=**精确十进制文本**(任意长,禁 double):9007199254740993 < …994 不回退 | `comparebridge.test` SortKeyPrecisionParity | >2^53 不失序 |
| GQL 全序 `null < bool < array < string < number < object`(TCK Aggregation2 [11][12] 钉,**与 CIP 相反**——TCK 是可执行规范);数字跨 INT/DOUBLE 按数值、胜出元素保类型;列表字典序 | `orderability.test` MixedValuesMaxMinParity / ListValueMaxMinParity;`comparebridge.test` CrossClassOrderParity / ArrayLexOrderParity | `_gql_max`/`_gql_min`/`_gql_sortkey` 统一全序 |
| 结构异的对象/DATE/UUID/INTERVAL/DECIMAL/STRUCT/MAP/非有限 real **无序=响亮拒**,不猜 | `comparebridge.test` SortKeyObjectRejected;README #21 | — |
| SQL NULL 三值:`NOT(UNKNOWN)`=UNKNOWN(`NOT(p.age=null)`→0 行) | `comparebridge.test` ThreeValuedLogicParity | — |
| 布尔拼写双形态:引擎 BOOL→JSON `True`/`False`;GQL INSERT 存小写 `true`/`false`——比较桥两拼写都归 bool | Phase 10 记录 | 形态判别必须先于解析 |
| ANY 图字符串属性存**裸文本**(`x` 不带引号),`_gql_to_json('x')` 产出 `"x"` 带引号——同列共存双形态;解析失败=裸字符串(Q2 验证修正①) | `comparebridge.test` TypedOperandsParity;Q2 回帖验证结论 | sortkey/桥解析步必须形态判别 |
| 算术无静默错(按另一侧类型数值化,不匹配响亮 ConversionException);CAST 不静默截断 | Phase 10 探机 | 算术面无需桥 |
| 裸文本恰巧拼成 JSON(字符串 `'true'`)——存储消型的内在不歧义,**parse-first 规则胜出** | README #21 | 已知边界,非静默错 |
| 空聚合组=**NULL 非 0**、multiplicity 循环加、跳 SQL NULL 与 JSON null、非数值响亮抛 | `jsonagg.test` SumNullSemanticsParity / SumMultiplicityParity / NonNumericSumRejected | `_GQL_SUM`/`_GQL_AVG`/`_GQL_MAX`/`_GQL_MIN`(扩展聚合,JSON→JSON 保型) |
| 节点表合成主键 `_gql_id SERIAL`(GQL 节点无键属性;`_ID` 是引擎保留字);NOT NULL 接受后丢弃 | README #11/#12 | 固定 schema 模型,与 REMOVE≈SET NULL 同族(README #1) |

## 四、语法面真话(两语法各自有什么/没有什么)

| 事实 | 证据 | 影响面 |
|---|---|---|
| GQL.g4(vendored opengql,3774 行)**语法已齐**(SELECT/GROUP BY/HAVING/写/事务/会话/QPPI 全可 parse)——瓶颈全在 transformer,**不改语法不换 parser** | 交接文档四 | — |
| GQL.g4 **无**:MERGE(非 ISO!是 Cypher 扩展)、FETCH、REPEAT…UNTIL、**列表下标 `x[0]`**(valueExpressionPrimary 只链 PERIOD)、`LENGTH()`、裸 `TIME`、**`IN` 谓词**、`IN`/`OUT` 关键字拼写之外的多标签冒号链 | 本轮探机(`x[0]` parse 即拒)+ GQL.g4 词法;`unsupported.test` UnsupportedMergeIsNotGql | 这些语法面问题在 parse 就响亮,无需翻译层兜 |
| Cypher.g4 **无 REMOVE 规则**(→`SET NULL` 近似);有 `iC_Transaction`(无裸 BEGIN)、`iC_RecursiveType`(SHORTEST/TRAIL/ACYCLIC)、USE/DROP/CREATE GRAPH 原生 DDL | 交接文档四;README #1 | — |
| ISO GROUP BY 分组键**只允许绑定变量**(`GROUP BY n`),表达式分组靠 SELECT 列表隐式分组 | 交接文档四 | — |
| CREATE GRAPH(GQL)**强制类型子句**(`ANY` 或 type spec);`CREATE GRAPH g TYPE t` 是宽容归一化;`CREATE GRAPH TYPE t AS COPY OF u` 文法歧义重解释 | README #14 | — |
| GQL 无裸 `SHORTEST`、无裸 `TIME`;属性名 `at` 是关键字 | 交接文档四 | — |
| `CALL GQL` 执行链:bindFunc(ANTLR 解析+翻译)→ rewriteFunc(整句替换 Cypher 文本)→ ClientContext 重解析;**多条 Cypher 只有最后一条的结果对外可见** | 交接文档四 | 事务包裹体因此拒(README #4);"CALL GQL 必须单独成句"是 standalone-call rewrite 真 bug 修复的根因 |
| GQL **INSERT 同变量多 pattern 合并成单节点**(造数必须变量互异) | 探机(测试工具坑) | 测试造数陷阱 |
| ANSI SQL 风格 `SELECT … FROM <graph>` → `USE GRAPH`+`MATCH…RETURN`;`USE` 是前缀从句(GQL.g4:382) | Q4 记录;README #22 | 见第一类 USE 陷阱 |
| ANTLR **锁 4.13.1**(4.13.2 parse-tree 层级不兼容会 downCast 崩) | 交接文档四 | 勿升级 |

### 附录 A:引擎原语探机事实(勿重复探)

- MATCH 允许边重复;`*TRAIL`=边互异;`*ACYCLIC`=仅中间点互异(见第二类)。
- var-length 边绑定值 = RECURSIVE_REL `{_NODES,_RELS}`;`relationships(e)`→rel 列表。
- var-length 属性图 `[e*2.. {p:10}]` = **所有边**语义(== GQL per-rep)。
- `WITH *, [a,b] AS ns, [e] AS es` 可用;节点等值 `a=b` 可用(下界 0 多跳的备选机器,有意不用)。
- UNION ALL 仅 RegularQuery 顶层(无包装从句);`USE GRAPH g; Q1 UNION ALL Q2` 批可行;
  **裸 `UNION` = appendDistinct 全局去重**(DISTINCT 投影的多跳区间形用它)。
- `LENGTH` 列表实参响亮拒(只有路径值);`size()` 吃列表。
- ORDER BY:聚合后只认投影别名;`ORDER BY 别名` 与模式变量同名会解析到模式变量(别名避开);
  DESC 下 NULL 排最前(引擎惯例)。
- NOT 三值逻辑成立(见第三类)。

### 附录 B:e2e 测试工具面坑(勿重复踩)

- e2e 文件名**下划线开头不注册**;组名 = 路径 `~` 连接(`gql~test~test_files~select`);
  运行器只在 argv[1] 是 `--gtest_filter=`/`--gtest_list_tests` 时注册——裸跑 0 tests。
- `.test` 格式:header `-DATASET CSV empty` + `-BUFFER_POOL_SIZE …` + 一条 `--` 行;
  CASE 间空行;`|` 多列拼接;无 `-CHECK_ORDER`=双方排序比较;`---- error` 精确(前缀
  `Runtime exception: `);`---- N` 后紧跟 N 行期望(勿拿 `---- 99` 探机)。
- 属性名 `id` 撞引擎内部;MSVC gtest 失败标记 `(228): error:`。
- TCK 转换器坑:Gherkin 结果表不是 docstring(单独抓 `|` 行);`FOR a..FOR b..RETURN`
  是一条语句;double 格式 %.6f、bool True/False、null=空串;**Bash heredoc 吃 C++ 转义**
  (写 C++ 字面量用 Edit 工具)。
- harness 裸标量不归一(`5`≠`5.0`);JSON 值打印=存储原文;`canonicalizeBareNumber` 剥尾零。

---

## 使用规则

1. **新事实必落测试**:同一提交 = 测试钉 + 本表行(+ README 差异条目如属用户可见面)。
2. **表行是断言不是描述**:改翻译层前查本表对应行;行为变化必须先改表再改码(或同提交)。
3. 本表只收"是什么";"怎么验证/怎么处置问题语料/怎么写简报"见 `docs/gql_methodology.md`;
   分阶段历史与未完成待办见 `_HANDOVER_GQL.md` 五/六节。
4. 署名链永久件:`extension/gql/THIRD_PARTY_NOTICES.md`、`docs/gql_ref/README.md`
   (Consulted but not kept)**永不删**。移交包 = 本表 + 方法学 + tests + notices 四件套。
