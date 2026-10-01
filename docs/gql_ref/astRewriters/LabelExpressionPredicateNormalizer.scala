/*
 * Copyright (c) "Neo4j"
 * Neo4j Sweden AB [https://neo4j.com]
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
package org.neo4j.cypher.internal.rewriting.rewriters.astRewriters

import org.neo4j.cypher.internal.CypherVersion
import org.neo4j.cypher.internal.ast.semantics.SemanticState
import org.neo4j.cypher.internal.label_expressions.LabelExpressionPredicate
import org.neo4j.cypher.internal.rewriting.conditions.ContainsNoMatchingStatementNodes
import org.neo4j.cypher.internal.rewriting.conditions.SemanticInfoAvailable
import org.neo4j.cypher.internal.rewriting.rewriters.LabelExpressionNormalizer
import org.neo4j.cypher.internal.rewriting.rewriters.factories.ASTRewriterFactory
import org.neo4j.cypher.internal.util.ASTNode
import org.neo4j.cypher.internal.util.AnonymousVariableNameGenerator
import org.neo4j.cypher.internal.util.CancellationChecker
import org.neo4j.cypher.internal.util.Rewriter
import org.neo4j.cypher.internal.util.StepSequencer
import org.neo4j.cypher.internal.util.symbols.ParameterTypeInfo
import org.neo4j.cypher.internal.util.topDown

case object containsNoLabelExpressionPredicates extends ContainsNoMatchingStatementNodes {

  override val matcher: PartialFunction[ASTNode, String] = {
    case pattern: LabelExpressionPredicate => pattern.asCanonicalStringVal
  }

  override val name: String = "NoLabelExpressionPredicates"
}

case object LabelExpressionPredicateNormalizer extends StepSequencer.Step with ASTRewriterFactory {

  val instance: Rewriter = topDown(Rewriter.lift {
    case pred: LabelExpressionPredicate => LabelExpressionNormalizer(pred.entity, None)(pred.labelExpression)
  })

  override def getRewriter(
    semanticState: SemanticState,
    parameterTypeMapping: Map[String, ParameterTypeInfo],
    anonymousVariableNameGenerator: AnonymousVariableNameGenerator,
    cancellationChecker: CancellationChecker,
    version: CypherVersion
  ): Rewriter = instance

  /**
   * @return the conditions that needs to be met before this step can be allowed to run.
   */
  override def preConditions: Set[StepSequencer.Condition] = Set.empty

  /**
   * @return the conditions that are guaranteed to be met after this step has run.
   *         Must not be empty, and must not contain any elements that are postConditions of other steps.
   */
  override def postConditions: Set[StepSequencer.Condition] = Set(
    containsNoLabelExpressionPredicates
  )

  /**
   * @return the conditions that this step invalidates as a side-effect of its work.
   */
  override def invalidatedConditions: Set[StepSequencer.Condition] = Set(
    NormalizeHasLabelsAndHasType.completed
  ) ++ SemanticInfoAvailable
}
