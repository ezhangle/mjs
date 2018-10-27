#include "interpreter.h"
#include "parser.h"
#include "global_object.h"

#include <sstream>
#include <algorithm>
#include <cmath>

#ifndef NDEBUG
#include <iostream>
#endif

namespace mjs {

std::wostream& operator<<(std::wostream& os, const completion_type& t) {
    switch (t) {
    case completion_type::normal: return os << "Normal completion";
    case completion_type::break_: return os << "Break";
    case completion_type::continue_: return os << "Continue";
    case completion_type::return_: return os << "Return";
    }
    NOT_IMPLEMENTED((int)t);
}

std::wostream& operator<<(std::wostream& os, const completion& c) {
    return os << c.type << " " << c.result;
}

class hoisting_visitor {
public:
    static std::vector<string> scan(const block_statement& bs) {
        hoisting_visitor hv{};
        hv(bs);
        return hv.ids_;
    }

    void operator()(const block_statement& s) {
        for (const auto& bs: s.l()) {
            accept(*bs, *this);
        }
    }

    void operator()(const variable_statement& s) {
        for (const auto& d: s.l()) {
            ids_.push_back(d.id());
        }
    }

    void operator()(const empty_statement&) {}

    void operator()(const expression_statement&){}

    void operator()(const if_statement& s) {
        accept(s.if_s(), *this);
        if (auto e = s.else_s()) {
            accept(*e, *this);
        }
    }

    void operator()(const while_statement& s){
        accept(s.s(), *this);
    }

    void operator()(const for_statement& s){
        if (s.init()) accept(*s.init(), *this);
        accept(s.s(), *this);
    }

    //void operator()(const for_in_statement&){}
    void operator()(const continue_statement&){}
    void operator()(const break_statement&){}
    void operator()(const return_statement&){}
    void operator()(const with_statement&){}

    void operator()(const function_definition& s) {
        assert(!s.id().view().empty());
        ids_.push_back(s.id());
    }

    void operator()(const statement& s) {
        NOT_IMPLEMENTED(s);
    }

private:
    explicit hoisting_visitor() {}
    std::vector<string> ids_;
};

class eval_exception : public std::runtime_error {
public:
    explicit eval_exception(const std::vector<source_extend>& stack_trace, const std::wstring_view& msg) : std::runtime_error(get_repr(stack_trace, msg)) {
    }

private:
    static std::string get_repr(const std::vector<source_extend>& stack_trace, const std::wstring_view& msg) {
        std::ostringstream oss;
        oss << std::string(msg.begin(), msg.end());
        for (const auto& e: stack_trace) {
            assert(e.file);
            oss << '\n' << e;
        }
        return oss.str();
    }
};

class interpreter::impl {
public:
    explicit impl(const block_statement& program, const on_statement_executed_type& on_statement_executed) : global_(global_object::make()), on_statement_executed_(on_statement_executed) {
        assert(!global_->has_property(string{"eval"}));
        global_->put(string{"eval"}, value{global_->make_function(
            [this](const value&, const std::vector<value>& args) {
            if (args.empty()) {
                return value::undefined;
            } else if (args.front().type() != value_type::string) {
                return args.front();
            }
            auto bs = parse(std::make_shared<source_file>(L"eval", args.front().string_value().view()));
            completion ret;
            for (const auto& s: bs->l()) {
                ret = eval(*s);
                if (ret) {
                    return value::undefined;
                }
            }
            assert(!ret);
            return ret.result;
        }, 1)}, property_attribute::dont_enum);

        for (const auto& id: hoisting_visitor::scan(program)) {
            global_->put(id, value::undefined);
        }

        scopes_.reset(new scope{global_, nullptr});
    }

    ~impl() {
        assert(scopes_ && !scopes_->prev);
#ifndef NDEBUG
        scopes_.reset();
        global_.reset();
        mjs::object::garbage_collect({});
        assert(!mjs::object::object_count());
#endif
    }

    value eval(const expression& e) {
        return accept(e, *this);
    }

    completion eval(const statement& s) {
        auto res = accept(s, *this);
        if (on_statement_executed_) {
            on_statement_executed_(s, res);
        }
        return res;
    }

    value operator()(const identifier_expression& e) {
        // �10.1.4
        return value{scopes_->lookup(e.id())};
    }

    value operator()(const literal_expression& e) {
        switch (e.t().type()) {
        case token_type::undefined_:      return value::undefined;
        case token_type::null_:           return value::null;
        case token_type::true_:           return value{true};
        case token_type::false_:          return value{false};
        case token_type::numeric_literal: return value{e.t().dvalue()};
        case token_type::string_literal:  return value{e.t().text()};
        default: NOT_IMPLEMENTED(e);
        }
    }

    value operator()(const call_expression& e) {
        auto member = eval(e.member());
        auto mval = get_value(member);
        auto args = eval_argument_list(e.arguments());
        if (mval.type() != value_type::object) {
            std::wostringstream woss;
            woss << e.member() << " is not a function";
            throw eval_exception(stack_trace(e.extend()), woss.str());
        }
        auto c = mval.object_value()->call_function();
        if (!c) {
            std::wostringstream woss;
            woss << e.member() << " is not callable";
            throw eval_exception(stack_trace(e.extend()), woss.str());
        }
        auto this_ = value::null;
        if (member.type() == value_type::reference) {
            if (auto o = member.reference_value().base(); o->class_name().view() != L"Activation") {
                this_ = value{o};
            }
        }

        scopes_->call_site = e.extend();
        auto res = c(this_, args);
        scopes_->call_site = source_extend{nullptr,0,0};
        return res;
    }

    value operator()(const prefix_expression& e) {
        if (e.op() == token_type::new_) {
            return handle_new_expression(e.e());
        }

        auto u = eval(e.e());
        if (e.op() == token_type::delete_) {
            if (u.type() != value_type::reference) {
                NOT_IMPLEMENTED(u);
            }
            const auto& base = u.reference_value().base();
            const auto& prop = u.reference_value().property_name();
            if (!base) {
                return value{true};
            }
            return value{base->delete_property(prop)};
        } else if (e.op() == token_type::void_) {
            (void)get_value(u);
            return value::undefined;
        } else if (e.op() == token_type::typeof_) {
            if (u.type() == value_type::reference && !u.reference_value().base()) {
                return value{string{"undefined"}};
            }
            u = get_value(u);
            switch (u.type()) {
            case value_type::undefined: return value{string{"undefined"}};
            case value_type::null: return value{string{"object"}};
            case value_type::boolean: return value{string{"boolean"}};
            case value_type::number: return value{string{"number"}};
            case value_type::string: return value{string{"string"}};
            case value_type::object: return value{string{u.object_value()->call_function() ? "function" : "object"}};
            default:
                NOT_IMPLEMENTED(u.type());
            }
        } else if (e.op() == token_type::plusplus || e.op() == token_type::minusminus) {
            if (u.type() != value_type::reference) {
                NOT_IMPLEMENTED(u);
            }
            auto num = to_number(get_value(u)) + (e.op() == token_type::plusplus ? 1 : -1);
            if (!put_value(u, value{num})) {
                NOT_IMPLEMENTED(u);
            }
            return value{num};
        } else if (e.op() == token_type::plus) {
            return value{to_number(get_value(u))};
        } else if (e.op() == token_type::minus) {
            return value{-to_number(get_value(u))};
        } else if (e.op() == token_type::tilde) {
            return value{static_cast<double>(~to_int32(get_value(u)))};
        } else if (e.op() == token_type::not_) {
            return value{!to_boolean(get_value(u))};
        }
        NOT_IMPLEMENTED(e);
    }

    value operator()(const postfix_expression& e) {
        auto member = eval(e.e());
        if (member.type() != value_type::reference) {
            NOT_IMPLEMENTED(e);
        }

        auto orig = to_number(get_value(member));
        auto num = orig;
        switch (e.op()) {
        case token_type::plusplus:   num += 1; break;
        case token_type::minusminus: num -= 1; break;
        default: NOT_IMPLEMENTED(e.op());
        }
        if (!put_value(member, value{num})) {
            NOT_IMPLEMENTED(e);
        }
        return value{orig};
    }

    // 0=false, 1=true, -1=undefined
    static int tri_compare(double l, double r) {
        if (std::isnan(l) || std::isnan(r)) {
            return -1;
        }
        if (l == r || (l == 0.0 && r == 0.0))  {
            return 0;
        }
        if (l == +INFINITY) {
            return 0;
        } else if (r == +INFINITY) {
            return 1;
        } else if (r == -INFINITY) {
            return 0;
        } else if (l == -INFINITY) {
            return 1;
        }
        return l < r;
    }
    static bool compare_equal(const value& l, const value& r) {
        if (l.type() == r.type()) {
            if (l.type() == value_type::undefined || l.type() == value_type::null) {
                return true;
            } else if (l.type() == value_type::number) {
                const double ln = l.number_value();
                const double rn = r.number_value();
                if (std::isnan(ln) || std::isnan(rn)) {
                    return false;
                }
                if ((ln == 0.0 && rn == 0.0) || ln == rn) {
                    return true;
                }
                return false;
            } else if (l.type() == value_type::string) {
                return l.string_value() == r.string_value();
            } else if (l.type() == value_type::boolean) {
                return l.boolean_value() == r.boolean_value();
            }
            assert(l.type() == value_type::object);
            return l.object_value() == r.object_value();
        }
        // Types are different
        if (l.type() == value_type::null && r.type() == value_type::undefined) {
            return true;
        } else if (l.type() == value_type::undefined && r.type() == value_type::null) {
            return true;
        } else if (l.type() == value_type::number && r.type() == value_type::string) {
            return compare_equal(l, value{to_number(r.string_value())});
        } else if (l.type() == value_type::string && r.type() == value_type::number) {
            return compare_equal(value{to_number(l.string_value())}, r);
        } else if (l.type() == value_type::boolean) {
            return compare_equal(value{static_cast<double>(l.boolean_value())}, r);
        } else if (r.type() == value_type::boolean) {
            return compare_equal(l, value{static_cast<double>(r.boolean_value())});
        } else if ((l.type() == value_type::string || l.type() == value_type::number) && r.type() == value_type::object) {
            return compare_equal(l, to_primitive(r));
        } else if ((r.type() == value_type::string || r.type() == value_type::number) && l.type() == value_type::object) {
            return compare_equal(to_primitive(l), r);
        }
        return false;
    }

    static value do_binary_op(const token_type op, value& l, value& r) {
        if (op == token_type::plus) {
            l = to_primitive(l);
            r = to_primitive(r);
            if (l.type() == value_type::string || r.type() == value_type::string) {
                auto ls = to_string(l);
                auto rs = to_string(r);
                return value{ls + rs};
            }
            // Otherwise handle like the other operators
        } else if (is_relational(op)) {
            l = to_primitive(l, value_type::number);
            r = to_primitive(r, value_type::number);
            if (l.type() == value_type::string && r.type() == value_type::string) {
                // TODO: See �11.8.5 step 16-21
                NOT_IMPLEMENTED(op);
            }
            const auto ln = to_number(l);
            const auto rn = to_number(r);
            int res;
            switch (op) {
            case token_type::lt:
                res = tri_compare(ln, rn);
                return value{res == -1 ? false : static_cast<bool>(res)};
            case token_type::ltequal:
                res = tri_compare(rn, ln);
                return value{res == -1 || res == 1 ? false : true};
            case token_type::gt:
                res = tri_compare(rn, ln);
                return value{res == -1 ? false : static_cast<bool>(res)};
            case token_type::gtequal:
                res = tri_compare(ln, rn);
                return value{res == -1 || res == 1 ? false : true};
            default: NOT_IMPLEMENTED(op);
            }
        } else if (op == token_type::equalequal || op == token_type::notequal) {
            const bool eq = compare_equal(l ,r);
            return value{op == token_type::equalequal ? eq : !eq};
        }

        const auto ln = to_number(l);
        const auto rn = to_number(r);
        switch (op) {
        case token_type::plus:         return value{ln + rn};
        case token_type::minus:        return value{ln - rn};
        case token_type::multiply:     return value{ln * rn};
        case token_type::divide:       return value{ln / rn};
        case token_type::mod:          return value{std::fmod(ln, rn)};
        case token_type::lshift:       return value{static_cast<double>(to_int32(ln) << (to_uint32(rn) & 0x1f))};
        case token_type::rshift:       return value{static_cast<double>(to_int32(ln) >> (to_uint32(rn) & 0x1f))};
        case token_type::rshiftshift:  return value{static_cast<double>(to_uint32(ln) >> (to_uint32(rn) & 0x1f))};
        case token_type::and_:         return value{static_cast<double>(to_int32(ln) & to_int32(rn))};
        case token_type::xor_:         return value{static_cast<double>(to_int32(ln) ^ to_int32(rn))};
        case token_type::or_:          return value{static_cast<double>(to_int32(ln) | to_int32(rn))};
        default: NOT_IMPLEMENTED(op);
        }
    }

    value operator()(const binary_expression& e) {
        if (e.op() == token_type::comma) {
            (void)get_value(eval(e.lhs()));;
            return get_value(eval(e.rhs()));
        }
        if (operator_precedence(e.op()) == assignment_precedence) {
            auto l = eval(e.lhs());
            auto r = get_value(eval(e.rhs()));
            if (e.op() != token_type::equal) {
                auto lval = get_value(l);
                r = do_binary_op(without_assignment(e.op()), lval, r);
            }
            if (!put_value(l, r)) {
                NOT_IMPLEMENTED(e);
            }
            return r;
        }

        auto l = get_value(eval(e.lhs()));
        if ((e.op() == token_type::andand && !to_boolean(l)) || (e.op() == token_type::oror && to_boolean(l))) {
            return l;
        }
        auto r = get_value(eval(e.rhs()));
        if (e.op() == token_type::andand || e.op() == token_type::oror) {
            return r;
        }
        if (e.op() == token_type::dot || e.op() == token_type::lbracket) {
            return value{reference{global_->to_object(l), to_string(r)}};
        }
        return do_binary_op(e.op(), l, r);
    }

    value operator()(const conditional_expression& e) {
        if (to_boolean(get_value(eval(e.cond())))) {
            return get_value(eval(e.lhs()));
        } else {
            return get_value(eval(e.rhs()));
        }
    }

    value operator()(const expression& e) {
        NOT_IMPLEMENTED(e);
    }

    //
    // Statements
    //

    completion operator()(const block_statement& s) {
        for (const auto& bs: s.l()) {
            if (auto c = eval(*bs)) {
                return c;
            }
        }
        return completion{};
    }

    completion operator()(const variable_statement& s) {
        for (const auto& d: s.l()) {
            assert(scopes_->activation->has_property(d.id()));
            if (d.init()) {
                scopes_->activation->put(d.id(), eval(*d.init()));
            }
        }
        return completion{};
    }

    completion operator()(const empty_statement&) {
        return completion{};
    }

    completion operator()(const expression_statement& s) {
        return completion{completion_type::normal, get_value(eval(s.e()))};
    }

    completion operator()(const if_statement& s) {
        if (to_boolean(get_value(eval(s.cond())))) {
            return eval(s.if_s());
        } else if (auto e = s.else_s()) {
            return eval(*e);
        }
        return completion{};
    }

    completion operator()(const while_statement& s) {
        while (to_boolean(get_value(eval(s.cond())))) {
            auto c = eval(s.s());
            if (c.type == completion_type::break_) {
                return completion{};
            } else if (c.type == completion_type::return_) {
                return c;
            }
            assert(c.type == completion_type::normal || c.type == completion_type::continue_);
        }
        return completion{};
    }

    completion operator()(const for_statement& s) {
        if (auto is = s.init()) {
            auto c = eval(*is);
            assert(!c); // Expect normal completion
            (void)get_value(c.result);
        }
        while (!s.cond() || to_boolean(get_value(eval(*s.cond())))) {
            auto c = eval(s.s());
            if (c.type == completion_type::break_) {
                return completion{};
            } else if (c.type == completion_type::return_) {
                return c;
            }
            assert(c.type == completion_type::normal || c.type == completion_type::continue_);

            if (s.iter()) {
                (void)get_value(eval(*s.iter()));
            }
        }
        return completion{};
    }

    //completion operator()(const for_in_statement&) {}

    completion operator()(const continue_statement&) {
        return completion{completion_type::continue_};
    }

    completion operator()(const break_statement&) {
        return completion{completion_type::break_};
    }

    completion operator()(const return_statement& s) {
        value res{};
        if (s.e()) {
            res = get_value(eval(*s.e()));
        }
        return completion{completion_type::return_, res};
    }

    completion operator()(const with_statement& s) {
        class with_scope {
        public:
            explicit with_scope(impl& parent, const object_ptr& o) : parent(parent), old_scopes(parent.scopes_) {
                parent.scopes_.reset(new scope{o, old_scopes});
            }
            ~with_scope() {
                parent.scopes_ = old_scopes;
            }
            impl& parent;
            scope_ptr old_scopes;
        };
        with_scope ws{*this, global_->to_object(get_value(eval(s.e())))};
        return eval(s.s());
    }

    completion operator()(const function_definition& s) {
        // �15.3.2.1
        auto prev_scope = scopes_;
        auto callee = global_->make_raw_function();
        auto func = [&s, this, prev_scope, callee, ids = hoisting_visitor::scan(s.block())](const value& this_, const std::vector<value>& args) {
            // Arguments array
            auto as = object::make(string{"Object"}, global_->object_prototype());
            as->put(string{"callee"}, value{callee}, property_attribute::dont_enum);
            as->put(string{"length"}, value{static_cast<double>(args.size())}, property_attribute::dont_enum);
            for (uint32_t i = 0; i < args.size(); ++i) {
                as->put(index_string(i), args[i], property_attribute::dont_enum);
            }

            // Scope
            auto_scope auto_scope_{*this, prev_scope};
            auto& scope = scopes_->activation;
            scope->put(string{"this"}, this_, property_attribute::dont_delete | property_attribute::dont_enum | property_attribute::read_only);
            scope->put(string{"arguments"}, value{as}, property_attribute::dont_delete);
            for (size_t i = 0; i < std::min(args.size(), s.params().size()); ++i) {
                scope->put(s.params()[i], args[i]);
            }
            // Variables
            for (const auto& id: ids) {
                assert(!scope->has_property(id)); // TODO: Handle this..
                scope->put(id, value::undefined);
            }
            return eval(s.block()).result;
        };
        global_->put_function(callee, func, static_cast<int>(s.params().size()));
        callee->construct_function([this, callee, name = s.id()](const value& unsused_this_, const std::vector<value>& args) {
            assert(unsused_this_.type() == value_type::undefined); (void)unsused_this_;
            assert(!name.view().empty());
            auto p = callee->get(string("prototype"));
            auto o = value{object::make(name, p.type() == value_type::object ? p.object_value() : global_->object_prototype())};
            auto r = callee->call_function()(o, args);
            return r.type() == value_type::object ? r : value{o};
        });

        prev_scope->activation->put(s.id(), value{callee});
        return completion{};
    }

    completion operator()(const statement& s) {
        NOT_IMPLEMENTED(s);
    }

private:
    class scope;
    using scope_ptr = std::shared_ptr<scope>;
    class scope {
    public:
        explicit scope(const object_ptr& act, const scope_ptr& prev) : activation(act), prev(prev) {}

        reference lookup(const string& id) const {
            if (!prev || activation->has_property(id)) {
                return reference{activation, id};
            }
            return prev->lookup(id);
        }

        object_ptr activation;
        scope_ptr prev;
        source_extend call_site;
    };
    class auto_scope {
    public:
        explicit auto_scope(impl& parent, const scope_ptr& prev) : parent(parent), old_scopes(parent.scopes_) {
            auto activation = object::make(string{"Activation"}, nullptr); // TODO
            parent.scopes_.reset(new scope{activation, prev});
        }
        ~auto_scope() {
            parent.scopes_ = old_scopes;
        }

        impl& parent;
        scope_ptr old_scopes;
    };
    scope_ptr                      scopes_;
    std::shared_ptr<global_object> global_;
    on_statement_executed_type     on_statement_executed_;

    std::vector<source_extend> stack_trace(const source_extend& current_extend) const {
        std::vector<source_extend> t;
        t.push_back(current_extend);
        for (const scope* p = scopes_.get(); p != nullptr; p = p->prev.get()) {
            if (!p->call_site.file) continue;
            t.push_back(p->call_site);
        }
        return t;
    }

    std::vector<value> eval_argument_list(const expression_list& es) {
        std::vector<value> args;
        for (const auto& e: es) {
            args.push_back(get_value(eval(*e)));
        }
        return args;
    }

    value handle_new_expression(const expression& e) {
        value o;
        std::vector<value> args;
        if (e.type() == expression_type::call) {
            const auto& ce = static_cast<const call_expression&>(e);
            o = eval(ce.member());
            args = eval_argument_list(ce.arguments());
        } else {
            o = eval(e);
        }
        o = get_value(o);
        if (o.type() != value_type::object) {
            std::wostringstream woss;
            woss << e << " is not an object";
            throw eval_exception(stack_trace(e.extend()), woss.str());
        }
        auto c = o.object_value()->construct_function();
        if (!c) {
            std::wostringstream woss;
            woss << e << " is not constructable";
            throw eval_exception(stack_trace(e.extend()), woss.str());
        }

        scopes_->call_site = e.extend();
        auto res = c(value::undefined, args);
        scopes_->call_site = source_extend{nullptr,0,0};
        return res;
    }
};

interpreter::interpreter(const block_statement& program, const on_statement_executed_type& on_statement_executed) : impl_(new impl{program, on_statement_executed}) {
}

interpreter::~interpreter() = default;

value interpreter::eval(const expression& e) {
    return impl_->eval(e);

}

completion interpreter::eval(const statement& s) {
    return impl_->eval(s);
}

} // namespace mjs
