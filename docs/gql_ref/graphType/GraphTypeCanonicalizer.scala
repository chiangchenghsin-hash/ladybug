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
// Source: neo4j/neo4j @ 54a7dcf7c2501b31866199143364c5332da8936f
// community/cypher/front-end/rewriting/src/main/scala/org/neo4j/cypher/internal/rewriting/rewriters/astRewriters/GraphTypeCanonicalizer.scala
package org.neo4j.cypher.internal.rewriting.rewriters.astRewriters

import org.neo4j.cypher.internal.CypherVersion
import org.neo4j.cypher.internal.ast.EdgeType
import org.neo4j.cypher.internal.ast.EdgeTypeReferenceByIdentifyingLabel
import org.neo4j.cypher.internal.ast.EdgeTypeReferenceByLabel
import org.neo4j.cypher.internal.ast.EmptyNodeTypeReference
import org.neo4j.cypher.internal.ast.GraphTypeConstraint.GraphTypeConstraintBody
import org.neo4j.cypher.internal.ast.GraphTypeConstraintDefinition
import org.neo4j.cypher.internal.ast.NodeType
import org.neo4j.cypher.internal.ast.NodeTypeReference
import org.neo4j.cypher.internal.ast.NodeTypeReferenceByIdentifyingLabel
import org.neo4j.cypher.internal.ast.NodeTypeReferenceByLabel
import org.neo4j.cypher.internal.ast.semantics.SemanticState
import org.neo4j.cypher.internal.expressions.Variable
import org.neo4j.cypher.internal.rewriting.conditions.GraphTypeCanonicalized
import org.neo4j.cypher.internal.rewriting.rewriters.factories.ASTRewriterFactory
import org.neo4j.cypher.internal.util.AnonymousVariableNameGenerator
import org.neo4j.cypher.internal.util.CancellationChecker
import org.neo4j.cypher.internal.util.InputPosition
import org.neo4j.cypher.internal.util.Rewriter
import org.neo4j.cypher.internal.util.StepSequencer
import org.neo4j.cypher.internal.util.StepSequencer.Condition
import org.neo4j.cypher.internal.util.bottomUp
import org.neo4j.cypher.internal.util.symbols.ParameterTypeInfo

case object GraphTypeCanonicalizer extends StepSequencer.Step with ASTRewriterFactory {

  override def preConditions: Set[StepSequencer.Condition] = Set()

  override def postConditions: Set[Condition] = Set(GraphTypeCanonicalized)

  override def invalidatedConditions: Set[Condition] = Set.empty

  override def getRewriter(
    semanticState: SemanticState,
    parameterTypeMapping: Map[String, ParameterTypeInfo],
    anonymousVariableNameGenerator: AnonymousVariableNameGenerator,
    cancellationChecker: CancellationChecker,
    version: CypherVersion
  ): Rewriter = instance

  val instance: Rewriter = bottomUp(Rewriter.lift {
    // Remove aliases from nodes
    case nt @ NodeType(variable, label, _, _, _) =>
      nt.copy(variable = None)(nt.position)

    // Remove aliases from edges
    case et @ EdgeType(
        src,
        variable,
        relType,
        _,
        dest,
        _
      ) =>
      et.copy(
        src = removeAliasFromReference(src),
        variable = None,
        dest = removeAliasFromReference(dest)
      )(et.position)

    // Rename variables in constraints
    case ct @ GraphTypeConstraintDefinition(_, ref @ NodeTypeReferenceByLabel(_, _), body, _) =>
      ct.copy(
        reference = ref.copy(typeReference = Some(nodeVariable))(ref.position),
        body = rewriteConstraintBodyProps(nodeVariable, body)
      )(ct.position)
    case ct @ GraphTypeConstraintDefinition(_, ref @ NodeTypeReferenceByIdentifyingLabel(_, _), body, _) =>
      ct.copy(
        reference = ref.copy(typeReference = Some(nodeVariable))(ref.position),
        body = rewriteConstraintBodyProps(nodeVariable, body)
      )(ct.position)
    case ct @ GraphTypeConstraintDefinition(_, ref @ EdgeTypeReferenceByLabel(_, _), body, _) =>
      ct.copy(
        reference = ref.copy(typeReference = Some(edgeVariable))(ref.position),
        body = rewriteConstraintBodyProps(edgeVariable, body)
      )(ct.position)
    case ct @ GraphTypeConstraintDefinition(_, ref @ EdgeTypeReferenceByIdentifyingLabel(_, _), body, _) =>
      ct.copy(
        reference = ref.copy(typeReference = Some(edgeVariable))(ref.position),
        body = rewriteConstraintBodyProps(edgeVariable, body)
      )(ct.position)
  })

  private val removeAliasFromReference: PartialFunction[NodeTypeReference, NodeTypeReference] = {
    case ntrl: NodeTypeReferenceByLabel            => ntrl.copy(typeReference = None)(ntrl.position)
    case ntrl: NodeTypeReferenceByIdentifyingLabel => ntrl.copy(typeReference = None)(ntrl.position)
    case e: EmptyNodeTypeReference                 => e
  }

  /**
   * Rewriter the properties in a constraint body to use a new variable
   */
  private def rewriteConstraintBodyProps(
    variable: Variable,
    constraintBody: GraphTypeConstraintBody
  ): GraphTypeConstraintBody =
    constraintBody.withProperties(constraintBody.properties.map(p => p.copy(map = variable)(p.position)))

  private val nodeVariable = Variable("n")(InputPosition.NONE, Variable.isIsolatedDefault)
  private val edgeVariable = Variable("r")(InputPosition.NONE, Variable.isIsolatedDefault)
}
