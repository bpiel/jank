#include <llvm/ExecutionEngine/Orc/LLJIT.h>

#include <jank/runtime/context.hpp>
#include <jank/runtime/ns.hpp>
#include <jank/runtime/obj/persistent_vector.hpp>
#include <jank/runtime/obj/number.hpp>
#include <jank/codegen/processor.hpp>
#include <jank/codegen/llvm_processor.hpp>
#include <jank/jit/processor.hpp>
#include <jank/evaluate.hpp>

namespace jank::evaluate
{
  using namespace jank::runtime;
  using namespace jank::analyze;

  /* Some expressions don't make sense to eval outright and aren't fns that can be JIT compiled.
   * For those, we wrap them in a fn expression and then JIT compile and call them.
   *
   * There's an oddity here, since that expr wouldn't've been analyzed within a fn frame, so
   * its lifted vars/constants, for example, aren't in a fn frame. Instead, they're put in the
   * root frame. So, when wrapping this expr, we give the fn the root frame, but change its
   * type to a fn frame. */
  template <typename E>
  expression_ptr wrap_expression(E expr)
  {
    auto ret(make_box<expression>(expr::function<expression>{}));
    auto &fn(boost::get<expr::function<expression>>(ret->data));
    expr::function_arity<expression> arity;
    fn.name = "repl_fn";
    fn.unique_name = context::unique_string(fn.name);
    fn.meta = obj::persistent_hash_map::empty();
    fn.frame = expr.frame;

    arity.frame = expr.frame;
    while(arity.frame->parent.is_some())
    {
      arity.frame = arity.frame->parent.unwrap();
    }
    arity.frame->type = local_frame::frame_type::fn;

    /* We can't just assign the position here, since we need the position to propagate
     * downward. For example, if this expr is a let, setting its position to tail
     * wouldn't affect the last form of its body, which should also be in tail position.
     *
     * This is what propagation does. */
    expr.propagate_position(expression_position::tail);

    /* TODO: Avoid allocation by using existing ptr. */
    arity.body.values.push_back(make_box<expression>(expr));
    arity.fn_ctx = make_box<expr::function_context>();
    arity.frame->fn_ctx = arity.fn_ctx;
    arity.body.frame = arity.frame;
    arity.fn_ctx->name = fn.name;
    arity.fn_ctx->unique_name = fn.unique_name;
    arity.fn_ctx->fn = ret;
    fn.arities.emplace_back(std::move(arity));

    return ret;
  }

  expression_ptr
  wrap_expressions(native_vector<expression_ptr> const &exprs, processor const &an_prc)
  {
    if(exprs.empty())
    {
      return wrap_expression(expr::primitive_literal<expression>{
        expression_base{ {}, expression_position::tail, an_prc.root_frame, true },
        obj::nil::nil_const()
      });
    }
    else
    {
      /* We'll cheat a little and build a fn using just the first expression. Then we can just
       * add the rest. I'd rather do this than duplicate all of the wrapping logic. */
      auto ret(wrap_expression(exprs[0]));
      auto &fn(boost::get<expr::function<expression>>(ret->data));
      auto &body(fn.arities[0].body.values);
      /* We normally wrap one expression, which is a return statement, but we'll be potentially
       * adding more, so let's not make assumptions yet. */
      body[0]->get_base()->position = expression_position::statement;

      for(auto const &expr : exprs)
      {
        body.emplace_back(expr);
      }

      /* Finally, mark the last body item as our return. */
      auto const last_body_index(body.size() - 1);
      body[last_body_index]->propagate_position(expression_position::tail);

      return ret;
    }
  }

  expression_ptr wrap_expression(expression_ptr const expr)
  {
    return boost::apply_visitor([](auto const &typed_expr) { return wrap_expression(typed_expr); },
                                expr->data);
  }

  object_ptr eval(context &rt_ctx, jit::processor const &jit_prc, expression_ptr const &ex)
  {
    profile::timer timer{ "eval ast node" };
    object_ptr ret{};
    boost::apply_visitor(
      [&rt_ctx, &jit_prc, &ret](auto const &typed_ex) { ret = eval(rt_ctx, jit_prc, typed_ex); },
      ex->data);

    assert(ret);
    return ret;
  }

  object_ptr eval(context &rt_ctx, jit::processor const &jit_prc, expr::def<expression> const &expr)
  {
    auto var(rt_ctx.intern_var(expr.name).expect_ok());
    var->meta = expr.name->meta;

    auto const meta(var->meta.unwrap_or(obj::nil::nil_const()));
    auto const dynamic(get(meta, rt_ctx.intern_keyword("dynamic").expect_ok()));
    var->set_dynamic(truthy(dynamic));

    if(expr.value.is_none())
    {
      return var;
    }

    auto const evaluated_value(eval(rt_ctx, jit_prc, expr.value.unwrap()));
    var->bind_root(evaluated_value);

    return var;
  }

  object_ptr eval(context &rt_ctx, jit::processor const &, expr::var_deref<expression> const &expr)
  {
    auto const var(rt_ctx.find_var(expr.qualified_name));
    return var.unwrap()->deref();
  }

  object_ptr eval(context &rt_ctx, jit::processor const &, expr::var_ref<expression> const &expr)
  {
    auto const var(rt_ctx.find_var(expr.qualified_name));
    return var.unwrap();
  }

  object_ptr
  eval(context &rt_ctx, jit::processor const &jit_prc, expr::call<expression> const &expr)
  {
    auto source(eval(rt_ctx, jit_prc, expr.source_expr));
    if(source->type == object_type::var)
    {
      source = deref(source);
    }

    return visit_object(
      [&](auto const typed_source) -> object_ptr {
        using T = typename decltype(typed_source)::value_type;

        if constexpr(std::is_base_of_v<behavior::callable, T>)
        {
          native_vector<object_ptr> arg_vals;
          arg_vals.reserve(expr.arg_exprs.size());
          for(auto const &arg_expr : expr.arg_exprs)
          {
            arg_vals.emplace_back(eval(rt_ctx, jit_prc, arg_expr));
          }

          /* TODO: Use apply_to */
          switch(arg_vals.size())
          {
            case 0:
              return dynamic_call(source);
            case 1:
              return dynamic_call(source, arg_vals[0]);
            case 2:
              return dynamic_call(source, arg_vals[0], arg_vals[1]);
            case 3:
              return dynamic_call(source, arg_vals[0], arg_vals[1], arg_vals[2]);
            case 4:
              return dynamic_call(source, arg_vals[0], arg_vals[1], arg_vals[2], arg_vals[3]);
            case 5:
              return dynamic_call(source,
                                  arg_vals[0],
                                  arg_vals[1],
                                  arg_vals[2],
                                  arg_vals[3],
                                  arg_vals[4]);
            case 6:
              return dynamic_call(source,
                                  arg_vals[0],
                                  arg_vals[1],
                                  arg_vals[2],
                                  arg_vals[3],
                                  arg_vals[4],
                                  arg_vals[5]);
            case 7:
              return dynamic_call(source,
                                  arg_vals[0],
                                  arg_vals[1],
                                  arg_vals[2],
                                  arg_vals[3],
                                  arg_vals[4],
                                  arg_vals[5],
                                  arg_vals[6]);
            case 8:
              return dynamic_call(source,
                                  arg_vals[0],
                                  arg_vals[1],
                                  arg_vals[2],
                                  arg_vals[3],
                                  arg_vals[4],
                                  arg_vals[5],
                                  arg_vals[6],
                                  arg_vals[7]);
            case 9:
              return dynamic_call(source,
                                  arg_vals[0],
                                  arg_vals[1],
                                  arg_vals[2],
                                  arg_vals[3],
                                  arg_vals[4],
                                  arg_vals[5],
                                  arg_vals[6],
                                  arg_vals[7],
                                  arg_vals[8]);
            case 10:
              return dynamic_call(source,
                                  arg_vals[0],
                                  arg_vals[1],
                                  arg_vals[2],
                                  arg_vals[3],
                                  arg_vals[4],
                                  arg_vals[5],
                                  arg_vals[6],
                                  arg_vals[7],
                                  arg_vals[8],
                                  arg_vals[9]);
            default:
              {
                /* TODO: This could be optimized; making lists sucks right now. */
                runtime::detail::native_persistent_list all{ arg_vals.rbegin(), arg_vals.rend() };
                for(size_t i{}; i < 10; ++i)
                {
                  all = all.rest();
                }
                return dynamic_call(source,
                                    arg_vals[0],
                                    arg_vals[1],
                                    arg_vals[2],
                                    arg_vals[3],
                                    arg_vals[4],
                                    arg_vals[5],
                                    arg_vals[6],
                                    arg_vals[7],
                                    arg_vals[8],
                                    arg_vals[9],
                                    make_box<obj::persistent_list>(all));
              }
          }
        }
        else if constexpr(std::same_as<T, obj::persistent_hash_set>
                          || std::same_as<T, obj::transient_vector>)
        {
          auto const s(expr.arg_exprs.size());
          if(s != 1)
          {
            throw std::runtime_error{
              fmt::format("invalid call with {} args to: {}", s, typed_source->to_string())
            };
          }
          return typed_source->call(eval(rt_ctx, jit_prc, expr.arg_exprs[0]));
        }
        else if constexpr(std::same_as<T, obj::keyword> || std::same_as<T, obj::persistent_hash_map>
                          || std::same_as<T, obj::persistent_array_map>
                          || std::same_as<T, obj::transient_hash_set>)
        {
          auto const s(expr.arg_exprs.size());
          switch(s)
          {
            case 1:
              return typed_source->call(eval(rt_ctx, jit_prc, expr.arg_exprs[0]));
            case 2:
              return typed_source->call(eval(rt_ctx, jit_prc, expr.arg_exprs[0]),
                                        eval(rt_ctx, jit_prc, expr.arg_exprs[1]));
            default:
              throw std::runtime_error{
                fmt::format("invalid call with {} args to: {}", s, typed_source->to_string())
              };
          }
        }
        else
        {
          throw std::runtime_error{ fmt::format("invalid call with 0 args to: {}",
                                                expr.arg_exprs.size(),
                                                typed_source->to_string()) };
        }
      },
      source);
  }

  object_ptr
  eval(context &rt_ctx, jit::processor const &, expr::primitive_literal<expression> const &expr)
  {
    if(expr.data->type == object_type::keyword)
    {
      auto const d(expect_object<obj::keyword>(expr.data));
      return rt_ctx.intern_keyword(d->sym.ns, d->sym.name).expect_ok();
    }
    return expr.data;
  }

  object_ptr
  eval(context &rt_ctx, jit::processor const &jit_prc, expr::vector<expression> const &expr)
  {
    runtime::detail::native_transient_vector ret;
    for(auto const &e : expr.data_exprs)
    {
      ret.push_back(eval(rt_ctx, jit_prc, e));
    }
    if(expr.meta.is_some())
    {
      return make_box<obj::persistent_vector>(expr.meta.unwrap(), ret.persistent());
    }
    else
    {
      return make_box<obj::persistent_vector>(ret.persistent());
    }
  }

  object_ptr eval(context &rt_ctx, jit::processor const &jit_prc, expr::map<expression> const &expr)
  {
    auto const size(expr.data_exprs.size());
    if(size <= obj::persistent_array_map::max_size)
    {
      auto const array_box(make_array_box<object_ptr>(size * 2));
      size_t i{};
      for(auto const &e : expr.data_exprs)
      {
        array_box.data[i++] = eval(rt_ctx, jit_prc, e.first);
        array_box.data[i++] = eval(rt_ctx, jit_prc, e.second);
      }

      if(expr.meta.is_some())
      {
        return make_box<obj::persistent_array_map>(expr.meta.unwrap(),
                                                   runtime::detail::in_place_unique{},
                                                   array_box,
                                                   size * 2);
      }
      else
      {
        return make_box<obj::persistent_array_map>(runtime::detail::in_place_unique{},
                                                   array_box,
                                                   size * 2);
      }
    }
    else
    {
      runtime::detail::native_transient_hash_map trans;
      for(auto const &e : expr.data_exprs)
      {
        trans.insert({ eval(rt_ctx, jit_prc, e.first), eval(rt_ctx, jit_prc, e.second) });
      }

      if(expr.meta.is_some())
      {
        return make_box<obj::persistent_hash_map>(expr.meta.unwrap(), trans.persistent());
      }
      else
      {
        return make_box<obj::persistent_hash_map>(trans.persistent());
      }
    }
  }

  object_ptr eval(context &rt_ctx, jit::processor const &jit_prc, expr::set<expression> const &expr)
  {
    runtime::detail::native_transient_hash_set ret;
    for(auto const &e : expr.data_exprs)
    {
      ret.insert(eval(rt_ctx, jit_prc, e));
    }
    if(expr.meta.is_some())
    {
      return make_box<obj::persistent_hash_set>(expr.meta.unwrap(), std::move(ret).persistent());
    }
    else
    {
      return make_box<obj::persistent_hash_set>(std::move(ret).persistent());
    }
  }

  object_ptr eval(context &, jit::processor const &, expr::local_reference const &)
  /* Doesn't make sense to eval these, since let is wrapped in a fn and JIT compiled. */
  {
    throw make_box("unsupported eval: local_reference");
  }

  object_ptr
  eval(context &rt_ctx, jit::processor const &jit_prc, expr::function<expression> const &expr)
  {
    auto const &module(
      module::nest_module(expect_object<ns>(rt_ctx.current_ns_var->deref())->to_string(),
                          munge(expr.unique_name)));

    /* TODO: Clean up. */
    auto const wrapped_expr(evaluate::wrap_expression(expr));
    codegen::llvm_processor cg_prc{ wrapped_expr, module, codegen::compilation_target::repl };
    cg_prc.gen();

    {
      profile::timer timer{ fmt::format("ir jit compile {}", expr.name) };
      //fmt::println("{}\n", cg_prc.to_string());
      llvm::cantFail(jit_prc.interpreter->getExecutionEngine().get().addIRModule(
        llvm::orc::ThreadSafeModule{ std::move(cg_prc.module), std::move(cg_prc.context) }));

      auto const init(jit_prc.interpreter->getSymbolAddress(cg_prc.ctor_name.c_str()).get());
      init.toPtr<void (*)()>()();

      auto const fn(
        jit_prc.interpreter->getSymbolAddress(fmt::format("{}_0", cg_prc.struct_name)).get());
      auto const ret(fn.toPtr<object *(*)()>()());
      return ret;
    }
  }

  object_ptr eval(context &, jit::processor const &, expr::recur<expression> const &)
  /* This will always be in a fn or loop, which will be JIT compiled. */
  {
    throw make_box("unsupported eval: recur");
  }

  object_ptr eval(context &, jit::processor const &, expr::recursion_reference<expression> const &)
  /* This will always be in a fn, which will be JIT compiled. */
  {
    throw make_box("unsupported eval: recursion_reference");
  }

  object_ptr eval(context &, jit::processor const &, expr::named_recursion<expression> const &)
  /* This will always be in a fn, which will be JIT compiled. */
  {
    throw make_box("unsupported eval: named_recursion");
  }

  object_ptr eval(context &rt_ctx, jit::processor const &jit_prc, expr::do_<expression> const &expr)
  {
    object_ptr ret{ obj::nil::nil_const() };
    for(auto const &form : expr.values)
    {
      ret = eval(rt_ctx, jit_prc, form);
    }
    return ret;
  }

  object_ptr eval(context &rt_ctx, jit::processor const &jit_prc, expr::let<expression> const &expr)
  {
    return dynamic_call(eval(rt_ctx, jit_prc, wrap_expression(expr)));
  }

  object_ptr eval(context &rt_ctx, jit::processor const &jit_prc, expr::if_<expression> const &expr)
  {
    auto const condition(eval(rt_ctx, jit_prc, expr.condition));
    if(truthy(condition))
    {
      return eval(rt_ctx, jit_prc, expr.then);
    }
    else if(expr.else_.is_some())
    {
      return eval(rt_ctx, jit_prc, expr.else_.unwrap());
    }
    return obj::nil::nil_const();
  }

  object_ptr
  eval(context &rt_ctx, jit::processor const &jit_prc, expr::throw_<expression> const &expr)
  {
    throw eval(rt_ctx, jit_prc, expr.value);
  }

  object_ptr
  eval(context &rt_ctx, jit::processor const &jit_prc, expr::try_<expression> const &expr)
  {
    return dynamic_call(eval(rt_ctx, jit_prc, wrap_expression(expr)));
  }

  object_ptr
  eval(context &rt_ctx, jit::processor const &jit_prc, expr::native_raw<expression> const &expr)
  {
    return dynamic_call(eval(rt_ctx, jit_prc, wrap_expression(expr)));
  }
}
