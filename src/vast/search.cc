#include "vast/search.h"

#include "vast/exception.h"
#include "vast/query.h"
#include "vast/util/make_unique.h"

using namespace cppa;

namespace vast {

search::search(actor_ptr archive, actor_ptr index, actor_ptr schema_manager)
  : archive_{std::move(archive)},
    index_{std::move(index)},
    schema_manager_{std::move(schema_manager)}
{
}

namespace {

// Deconstructs an entire query AST into its individual sub-trees.
class dissector : public expr::default_const_visitor
{
public:
  dissector(std::map<expr::ast, search::state>& state, expr::ast const& base)
    : state_{state},
      base_{base}
  {
  }

  virtual void visit(expr::conjunction const& conj)
  {
    expr::ast ast{conj};
    if (parent_)
      state_[ast].parents.insert(parent_);
    else if (ast == base_)
      state_.emplace(ast, search::state{});
    parent_ = std::move(ast);
    for (auto& clause : conj.operands)
      clause->accept(*this);
  }

  virtual void visit(expr::disjunction const& disj)
  {
    expr::ast ast{disj};
    if (parent_)
      state_[ast].parents.insert(parent_);
    else if (ast == base_)
      state_.emplace(ast, search::state{});
    parent_ = std::move(ast);
    for (auto& term : disj.operands)
      term->accept(*this);
  }

  virtual void visit(expr::relation const& rel)
  {
    assert(parent_);
    expr::ast ast{rel};
    state_[ast].parents.insert(parent_);
  }

private:
  expr::ast parent_;
  std::map<expr::ast, search::state>& state_;
  expr::ast const& base_;
};


struct node_tester : public expr::default_const_visitor
{
  virtual void visit(expr::conjunction const&)
  {
    type = logical_and;
  }

  virtual void visit(expr::disjunction const&)
  {
    type = logical_or;
  }

  boolean_operator type;
};


struct conjunction_updater : public expr::default_const_visitor
{
  conjunction_updater(std::map<expr::ast, search::state>& state)
    : state{state}
  {
  }

  virtual void visit(expr::conjunction const& conj)
  {
    for (auto& operand : conj.operands)
      operand->accept(*this);
  }

  virtual void visit(expr::disjunction const& disj)
  {
    update(disj);
  }

  virtual void visit(expr::relation const& rel)
  {
    update(rel);
  }

  void update(expr::ast const& child)
  {
    auto i = state.find(child);
    assert(i != state.end());
    auto& child_result = i->second.result;
    if (! result)
      result = child_result;
    else if (child_result)
      result &= child_result;
  }

  bitstream result;
  std::map<expr::ast, search::state> const& state;
};

} // namespace <anonymous>

void search::act()
{
  trap_exit(true);
  become(
      on(atom("EXIT"), arg_match) >> [=](uint32_t reason)
      {
        std::set<actor_ptr> doomed;
        for (auto& p : actors_)
        {
          doomed.insert(p.second.first);
          doomed.insert(p.second.second);
        }
        for (auto& d : doomed)
          send_exit(d, exit::stop);
        quit(reason);
      },
      on(atom("DOWN"), arg_match) >> [=](uint32_t)
      {
        VAST_LOG_ACTOR_DEBUG(
            "received DOWN from client @" << last_sender()->id());
        std::set<actor_ptr> doomed;
        for (auto& p : actors_)
        {
          if (p.second.second == last_sender())
          {
            doomed.insert(p.second.first);
            doomed.insert(p.second.second);
          }
        }
        for (auto& d : doomed)
          send_exit(d, exit::stop);
      },
      on(atom("query"), atom("create"), arg_match) >> [=](std::string const& q)
      {
        expr::ast ast{q};
        if (! ast)
        {
          reply(actor_ptr{}, ast);
          return;
        }
        monitor(last_sender());
        auto qry = spawn<query_actor>(archive_, last_sender(), ast);
        actors_.emplace(ast, std::make_pair(qry, last_sender()));
        dissector d{state_, ast};
        ast.accept(d);
        // TODO: reuse results from existing queries before asking index.
        send(index_, ast);
        reply(std::move(qry), std::move(ast));
      },
      on_arg_match >> [=](expr::ast const& ast,
                          bitstream const& result, bitstream const& coverage)
      {
        assert(state_.count(ast));
        if (! result)
        {
          VAST_LOG_ACTOR_DEBUG("got empty result for: " << ast);
          return;
        }
        VAST_LOG_ACTOR_DEBUG("got result for: " << ast);
        auto& s = state_[ast];
        // Update coverage.
        if (! s.coverage)
          s.coverage = std::move(coverage);
        else
          s.coverage |= coverage;
        // Update result.
        if (! s.result)
          s.result = std::move(result);
        else
          s.result |= result;
        for (auto& parent : s.parents)
        {
          assert(state_.count(parent));
          auto& parent_state = state_[parent];
          node_tester nt;
          parent.accept(nt);
          // Update the coverage of the parents.
          if (! parent_state.coverage)
            parent_state.coverage = s.coverage;
          else
            parent_state.coverage |= s.coverage;
          // Update the result of all parents.
          if (! parent_state.result)
          {
            parent_state.result = s.result;
          }
          else if (nt.type == logical_or)
          {
            parent_state.result |= s.result;
          }
          else if (nt.type == logical_and)
          {
            conjunction_updater cu{state_};
            parent.accept(cu);
            parent_state.result = std::move(cu.result);
          }
          // Update all related queries with the latest result.
          auto er = actors_.equal_range(parent);
          for (auto i = er.first; i != er.second; ++i)
          {
            VAST_LOG_ACTOR_DEBUG(
                "propagates new result for " << i->first <<
                " to query @" << i->second.first->id());
            send(i->second.first, s.result);
          }
        }
      },
      others() >> [=]
      {
        VAST_LOG_ACTOR_ERROR("got unexpected message from @" <<
                             last_sender()->id() << ": " <<
                             to_string(last_dequeued()));
      });
}

char const* search::description() const
{
  return "search";
}


} // namespace vast
