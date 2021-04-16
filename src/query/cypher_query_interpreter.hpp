#pragma once

//////////////////////////////////////////////////////
// THIS INCLUDE SHOULD ALWAYS COME BEFORE THE
// "cypher_main_visitor.hpp"
// "planner.hpp" includes json.hpp which uses libc's
// EOF macro while "cypher_main_visitor.hpp" includes
// "antlr4-runtime.h" which contains a static variable
// of the same name, EOF.
// This hides the definition of the macro which causes
// the compilation to fail.
#include "query/plan/planner.hpp"
//////////////////////////////////////////////////////
#include "query/frontend/ast/cypher_main_visitor.hpp"
#include "query/frontend/opencypher/parser.hpp"
#include "query/frontend/semantic/required_privileges.hpp"
#include "query/frontend/semantic/symbol_generator.hpp"
#include "query/frontend/stripped.hpp"
#include "utils/flag_validation.hpp"
#include "utils/timer.hpp"

// NOLINTNEXTLINE (cppcoreguidelines-avoid-non-const-global-variables)
DECLARE_bool(query_cost_planner);
// NOLINTNEXTLINE (cppcoreguidelines-avoid-non-const-global-variables)
DECLARE_int32(query_plan_cache_ttl);

namespace query {

// TODO: Maybe this should move to query/plan/planner.
/// Interface for accessing the root operator of a logical plan.
class LogicalPlan {
 public:
  explicit LogicalPlan() = default;

  virtual ~LogicalPlan() = default;

  LogicalPlan(const LogicalPlan &) = default;
  LogicalPlan &operator=(const LogicalPlan &) = default;
  LogicalPlan(LogicalPlan &&) = default;
  LogicalPlan &operator=(LogicalPlan &&) = default;

  virtual const plan::LogicalOperator &GetRoot() const = 0;
  virtual double GetCost() const = 0;
  virtual const SymbolTable &GetSymbolTable() const = 0;
  virtual const AstStorage &GetAstStorage() const = 0;
};

class CachedPlan {
 public:
  explicit CachedPlan(std::unique_ptr<LogicalPlan> plan);

  const auto &plan() const { return plan_->GetRoot(); }
  double cost() const { return plan_->GetCost(); }
  const auto &symbol_table() const { return plan_->GetSymbolTable(); }
  const auto &ast_storage() const { return plan_->GetAstStorage(); }

  bool IsExpired() const {
    // NOLINTNEXTLINE (modernize-use-nullptr)
    return cache_timer_.Elapsed() > std::chrono::seconds(FLAGS_query_plan_cache_ttl);
  };

 private:
  std::unique_ptr<LogicalPlan> plan_;
  utils::Timer cache_timer_;
};

struct CachedQuery {
  AstStorage ast_storage;
  Query *query;
  std::vector<AuthQuery::Privilege> required_privileges;
};

struct QueryCacheEntry {
  bool operator==(const QueryCacheEntry &other) const { return first == other.first; }
  bool operator<(const QueryCacheEntry &other) const { return first < other.first; }
  bool operator==(const uint64_t &other) const { return first == other; }
  bool operator<(const uint64_t &other) const { return first < other; }

  uint64_t first;
  // TODO: Maybe store the query string here and use it as a key with the hash
  // so that we eliminate the risk of hash collisions.
  CachedQuery second;
};

struct PlanCacheEntry {
  bool operator==(const PlanCacheEntry &other) const { return first == other.first; }
  bool operator<(const PlanCacheEntry &other) const { return first < other.first; }
  bool operator==(const uint64_t &other) const { return first == other; }
  bool operator<(const uint64_t &other) const { return first < other; }

  uint64_t first;
  // TODO: Maybe store the query string here and use it as a key with the hash
  // so that we eliminate the risk of hash collisions.
  std::shared_ptr<CachedPlan> second;
};

/**
 * A container for data related to the parsing of a query.
 */
struct ParsedQuery {
  std::string query_string;
  std::map<std::string, storage::PropertyValue> user_parameters;
  Parameters parameters;
  frontend::StrippedQuery stripped_query;
  AstStorage ast_storage;
  Query *query;
  std::vector<AuthQuery::Privilege> required_privileges;
  bool is_cacheable{true};
};

ParsedQuery ParseQuery(const std::string &query_string, const std::map<std::string, storage::PropertyValue> &params,
                       utils::SkipList<QueryCacheEntry> *cache, utils::SpinLock *antlr_lock);

class SingleNodeLogicalPlan final : public LogicalPlan {
 public:
  SingleNodeLogicalPlan(std::unique_ptr<plan::LogicalOperator> root, double cost, AstStorage storage,
                        const SymbolTable &symbol_table)
      : root_(std::move(root)), cost_(cost), storage_(std::move(storage)), symbol_table_(symbol_table) {}

  const plan::LogicalOperator &GetRoot() const override { return *root_; }
  double GetCost() const override { return cost_; }
  const SymbolTable &GetSymbolTable() const override { return symbol_table_; }
  const AstStorage &GetAstStorage() const override { return storage_; }

 private:
  std::unique_ptr<plan::LogicalOperator> root_;
  double cost_;
  AstStorage storage_;
  SymbolTable symbol_table_;
};

/**
 * Return the parsed *Cypher* query's AST cached logical plan, or create and
 * cache a fresh one if it doesn't yet exist.
 */
std::shared_ptr<CachedPlan> CypherQueryToPlan(uint64_t hash, AstStorage ast_storage, CypherQuery *query,
                                              const Parameters &parameters, utils::SkipList<PlanCacheEntry> *plan_cache,
                                              DbAccessor *db_accessor, bool is_cacheable = true,
                                              std::vector<Identifier *> predefined_identifiers = {});

}  // namespace query
