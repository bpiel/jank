#include <jank/translate/cell/cell.hpp>
#include <jank/translate/macro/emit_state.hpp>
#include <jank/translate/environment/scope.hpp>
#include <jank/interpret/interpret.hpp>
#include <jank/interpret/plugin/apply.hpp>

namespace jank
{
  namespace translate
  {
    namespace macro
    {
      /* For function calls, this does nothing. */
      template <typename T>
      T evaluate
      (
        T &&call,
        std::shared_ptr<environment::scope> const&
      )
      { return std::forward<T>(call); }

      /* TODO: Move to cpp */
      /* For macros, this interprets in place and stores the results. */
      inline cell::macro_call evaluate
      (
        cell::macro_call &&call,
        std::shared_ptr<environment::scope> const &scope
      )
      {
        auto const interpret_scope
        (std::make_shared<interpret::environment::scope>());
        interpret::plugin::apply(scope, interpret_scope);

        /* TODO: Only allow emit in macros. */
        {
          /* Calls to emit will populate this object. */
          emit_state emissions;

          interpret::interpret
          (
            interpret_scope,
            cell::function_body{ call.data.definition.body }
          );

          call.data.result = std::move(emissions.cells);
        }

        return std::move(call);
      }
    }
  }
}
