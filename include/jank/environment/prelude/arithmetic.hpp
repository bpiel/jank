#pragma once

#include <jtl/iterator/range.hpp>

#include <jank/cell/cell.hpp>
#include <jank/expect/type.hpp>
#include <jank/expect/arg.hpp>

namespace jank
{
  namespace environment
  {
    namespace prelude
    {
      namespace detail
      {
        enum class op
        {
          add,
          sub,
          mul,
          div
        };

        template <op O>
        auto apply(cell::integer::type const to, cell::integer::type const from);
        template <>
        inline auto apply<op::add>(cell::integer::type const to, cell::integer::type const from)
        { return to + from; }
        template <>
        inline auto apply<op::sub>(cell::integer::type const to, cell::integer::type const from)
        { return to - from; }
        template <>
        inline auto apply<op::mul>(cell::integer::type const to, cell::integer::type const from)
        { return to * from; }
        template <>
        inline auto apply<op::div>(cell::integer::type const to, cell::integer::type const from)
        { return to / from; }

        /* TODO: read idents from env */
        template <op O>
        cell::cell apply_all(state&, cell::list const &cl)
        {
          auto const list(cl.data);
          expect::at_least_args(cl, 2);

          cell::integer::type val
          {
            expect::type<cell::type::integer>
            (
              *std::next(list.begin())
            ).data
          };
          for(auto &i : jtl::it::make_range(std::next(list.begin(), 2), list.end()))
          { val = apply<O>(val, expect::type<cell::type::integer>(i).data); }

          return cell::integer{ val };
        }
      }

      inline cell::cell sum(state &env, cell::list const &cl)
      { return detail::apply_all<detail::op::add>(env, cl); }

      inline cell::cell difference(state &env, cell::list const &cl)
      { return detail::apply_all<detail::op::sub>(env, cl); }

      inline cell::cell product(state &env, cell::list const &cl)
      { return detail::apply_all<detail::op::mul>(env, cl); }

      inline cell::cell quotient(state &env, cell::list const &cl)
      { return detail::apply_all<detail::op::div>(env, cl); }
    }
  }
}
