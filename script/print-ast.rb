#! /usr/bin/env ruby

# Parse a source file using Ripper and print its AST in the form
# expected by tree-sitter test

require "ripper"
require "pp"

class Node
  attr_reader :tag, :children, :field
  attr_writer :tag, :field

  def initialize(tag:)
    @tag = tag
    @field = :__child__
    @children = []
  end

  def pp(indent = 0)
    unless @field.nil?
      field = @field == :__child__ ? "" : "#{@field.to_s}: "
      print "#{"\n" unless indent == 0}#{" " * indent}#{field}(#{@tag.to_s}"
      @children.each do |c|
        c.pp (indent + 2)
      end
      print ")"
    end
  end

  def value()
    children[0].tag.to_s
  end

  def add_child(child)
    add_field(:__child__, child)
  end

  def prepend_child(child)
    prepend_field(:__child__, child)
  end

  def add_token(value)
    add_field(nil, Node.new(tag: :"#{value}"))
  end

  def add_field(name, child)
    unless child.nil?
      child.field = name
      @children.append(child)
    end
    self
  end

  def prepend_field(name, child)
    unless child.nil?
      child.field = name
      @children.prepend(child)
    end
    self
  end

  def add_children(children)
    if Array === children
      children.each { |c| add_child(c) }
    elsif Node === children
      add_child(children)
    end
    self
  end
end

class AST < Ripper

  # Ripper::PARSER_EVENT_TABLE.each do |event, arity|
  #     print(<<-End)
  # def on_#{event}(#{(1..arity).to_a.map { |x| "a" + x.to_s}.join(', ')})
  #     puts :#{event}
  # end
  #     End
  # end
  def initialize(*args, **kwargs)
    super(*args, **kwargs)
    @string_stack = []
    @has_end = false
    @last_semicolon = []
    @oneline_pattern = :test_pattern
  end

  def on_BEGIN(stmts)
    strip_empty(stmts)
    Node.new(tag: :begin_block).add_children(stmts)
  end

  def on_END(stmts)
    strip_empty(stmts)
    Node.new(tag: :end_block).add_children(stmts)
  end

  def on_alias(left, right)
    Node.new(tag: :alias)
      .add_field(:name, method_name(left))
      .add_field(:alias, method_name(right))
  end

  def on_alias_error(message, node)
    print "#{self.lineno}: #{message}\n"
    node
  end

  def on_aref(collection, index)
    Node.new(tag: :element_reference)
      .add_field(:object, collection)
      .add_children(index)
  end

  def on_aref_field(collection, index)
    on_aref(collection, index)
  end

  def on_arg_ambiguous(value)
    value
  end

  def on_arg_paren(args)
    args
  end

  def on_args_add(args, arg)
    if arg.nil?
    elsif arg.tag == :bare_assoc_hash
      args.concat(arg.children)
    else
      args.append(arg) unless arg.nil?
    end
    args
  end

  def on_args_add_block(args, block)
    if block.nil?
      args.append(Node.new(tag: :block_argument))
    else
      args.append(Node.new(tag: :block_argument).add_child(block)) if block
    end
    args
  end

  def on_args_add_star(args, arg)
    children = []
    children.append(arg) unless args.nil?
    args.append(Node.new(tag: :splat_argument).add_child(arg))
  end

  def on_args_forward()
    Node.new(tag: :forward_argument)
  end

  def on_args_new()
    []
  end

  def on_array(contents)
    # nil, args_add, args_add_star, qwords_add, qsymbols_add, words_add, and symbols_add

    if contents.nil?
      Node.new(tag: :array)
    elsif Array === contents
      Node.new(tag: :array).add_children(contents)
    else
      if contents.tag == :string_array or contents.tag == :symbol_array
        @string_stack.pop()
      end
      contents
    end
  end

  def splatparameter_var_field(var_field)
    Node.new(tag: :splat_parameter).add_field(:name, var_field.children[0])
  end

  def on_aryptn(const, preargs, splatarg, postargs)
    node = Node.new(tag: :array_pattern)
    node.add_field(:class, const)
    node.add_children(preargs.map { |x| to_pattern x }) unless preargs.nil?
    node.add_child(splatparameter_var_field(splatarg)) unless splatarg.nil?
    node.add_children(postargs.map { |x| to_pattern x }) unless postargs.nil?
    node
  end

  def on_assign(left, right)
    if Array === right and right.length == 1
      right = right[0]
    end
    if left.tag == :var_field
      left = left.children[0]
    end
    Node.new(tag: :assignment)
      .add_field(:left, left)
      .add_field(:right, right)
  end

  def on_assign_error(message, node)
    print "#{self.lineno}: #{message}\n"
    node
  end

  def on_assoc_new(key, value)
    Node.new(tag: :pair)
      .add_field(:key, key)
      .add_field(:value, value)
  end

  def on_assoc_splat(contents)
    Node.new(tag: :hash_splat_argument).add_child(contents)
  end

  def on_assoclist_from_args(assocs)
    assocs
  end

  def on_bare_assoc_hash(assocs)
    Node.new(tag: :bare_assoc_hash).add_children(assocs)
  end

  def on_begin(stmts)
    stmts = [*stmts]
    strip_empty(stmts)
    Node.new(tag: :begin).add_children(stmts)
  end

  def on_binary(left, operator, right)
    Node.new(tag: :binary)
      .add_field(:left, left)
      .add_token(operator)
      .add_field(:right, right)
  end

  def on_block_var(params, locals)
    node = Node.new(tag: :block_parameters)
    node.add_children(to_parameters(params))
    locals.each { |l| node.add_field(:locals, l) } if locals
    node
  end

  def on_blockarg(ident)
    Node.new(tag: :block_parameter).add_field(:name, ident)
  end

  def on_bodystmt(stmts, rescued, elsed, ensured)
    @lamdba_block_kind = [:do_block, :body_statement]
    stmts.concat rescued unless rescued.nil?
    stmts.append Node.new(tag: :else).add_children(elsed) unless elsed.nil?
    stmts.append ensured unless ensured.nil?
    stmts
  end

  def on_brace_block(block_var, stmts)
    node = Node.new(tag: :block)
    node.add_field(:parameters, block_var) unless block_var.nil?
    node.add_field(:body, Node.new(tag: :block_body).add_children(stmts)) unless (stmts.nil? || stmts.empty?)
    node
  end

  def return_like(tag, arguments)
    if Node === arguments and arguments.tag == :parenthesized_statements
      arguments = arguments.children
    end
    node = Node.new(tag: tag)
    node.add_child(Node.new(tag: :argument_list).add_children(arguments)) unless (arguments.nil? or arguments.empty?)
    node
  end

  def on_break(arguments)
    return_like(:break, arguments)
  end

  def on_call(receiver, operator, message)
    node = Node.new(tag: :call)
      .add_field(:receiver, receiver)
      .add_token(operator) #TODO
    if Node === message && message.tag == :identifier && /[A-Z]/.match?(message.value[0])
       message.tag = :constant
    end
    node.add_field(:method, message) unless message == :call
    node
  end

  def on_case(switch, consequent)
    case_node = if consequent.nil? then Node.new(tag: :case) else consequent end
    case_node.prepend_field(:value, switch) unless switch.nil?
    case_node
  end

  def on_CHAR(value)
    Node.new(tag: :character).add_token(value)
  end

  def strip_empty(bodystmt)
    if Array === bodystmt and bodystmt.length >= 1 and bodystmt[0].tag == :empty_statement
      bodystmt.shift
    end
  end

  def on_class(const, superclass, bodystmt)
    strip_empty(bodystmt)
    node = Node.new(tag: :class)
      .add_field(:name, const)
    node.add_field(:superclass, Node.new(tag: :superclass).add_child(superclass)) unless superclass.nil?
    node.add_field(:body, Node.new(tag: :body_statement).add_children(bodystmt)) unless (bodystmt.nil? || bodystmt.empty?)
    node
  end

  def on_class_name_error(message, node)
    print "#{self.lineno}: #{message}\n"
    node
  end

  def on_command(message, args)
    on_command_call(nil, ".", message, args)
  end

  def on_command_call(receiver, operator, method, args)
    result = on_call(receiver, operator, method)
    result = on_method_add_arg(result, args)
    result
  end

  def on_const_path_field(left, const)
    on_const_path_ref(left, const)
  end

  def on_const_path_ref(left, const)
    Node.new(tag: :scope_resolution)
      .add_field(:scope, left)
      .add_field(:name, const)
  end

  def on_const_ref(const)
    const
  end

  def on_const(value)
    Node.new(tag: :constant).add_token(value)
  end

  def on_cvar(value)
    Node.new(tag: :class_variable).add_token(value)
  end

  def to_parameters(params)
    params.map { |p|
      if p.tag == :destructured_left_assignment
        children = to_parameters(p.children)
        Node.new(tag: :destructured_parameter).add_children(children)
      elsif p.tag == :rest_assignment
        node = Node.new(tag: :splat_parameter)
        node.add_field(:name, p.children[0]) unless p.children.empty?
        node
      else
        p
      end
    }
  end

  def on_def(ident, params, body)
    strip_empty(body)
    node = Node.new(tag: :method)
      .add_field(:name, method_name(ident))
    if Array === params
      node.add_field(:parameters, Node.new(tag: :method_parameters).add_children(to_parameters(params))) unless params.empty?
    elsif params.tag == :parenthesized_statements
      node.add_field(:parameters, Node.new(tag: :method_parameters).add_children(to_parameters(params.children)))
    end unless params.nil?
    body = [*body]
    node.add_field(:body, Node.new(tag: :body_statement).add_children(body)) unless body.empty?
    node
  end

  def on_defined(value)
    on_unary(:"defined?", value)
  end

  def on_defs(object, operator, ident, params, body)
    strip_empty(body)
    if object.tag == :parenthesized_statements
      object = object.children[0]
    end
    node = Node.new(tag: :singleton_method)
      .add_field(:object, object)
    #TODO: operator
      .add_field(:name, method_name(ident))
    if Array === params
      node.add_field(:parameters, Node.new(tag: :method_parameters).add_children(to_parameters(params))) unless params.empty?
    elsif params.tag == :parenthesized_statements
      node.add_field(:parameters, Node.new(tag: :method_parameters).add_children(to_parameters(params.children)))
    end unless params.nil?
    body = [*body]
    node.add_field(:body, Node.new(tag: :body_statement).add_children(body)) unless body.empty?
    node
  end

  def on_do_block(block_var, bodystmt)
    strip_empty(bodystmt)
    node = Node.new(tag: :do_block).add_field(:parameters, block_var)
    node.add_field(:body, Node.new(tag: :body_statement).add_children(bodystmt)) unless (bodystmt.nil? || bodystmt.empty?)
    node
  end

  def on_dot2(left, right)
    Node.new(tag: :range)
      .add_field(:begin, left)
      .add_field(:end, right)
  end

  def on_dot3(left, right)
    Node.new(tag: :range)
      .add_field(:begin, left)
      .add_field(:end, right)
  end

  def on_dyna_symbol(contents)
    @string_stack.pop()
    contents
  end

  def on_else(stmts)
    strip_empty(stmts)
    Node.new(tag: :else).add_children(stmts)
  end

  def on_elsif(predicate, then_stmts, else_stmts)
    node = Node.new(tag: :elsif)
      .add_field(:condition, predicate)
    node.add_field(:consequence, Node.new(tag: :then).add_children(then_stmts)) unless then_stmts.empty?
    node.add_field(:alternative, else_stmts)
    node
  end

  def on_ensure(stmts)
    Node.new(tag: :ensure).add_children(stmts)
  end

  def on_excessed_comma()
    # Node.new(tag: :splat_parameter)
    nil
  end

  def on_fcall(message)
    on_call(nil, ".", message)
  end

  def on_field(left, operator, right)
    on_command_call(left, operator, right, [])
  end

  def on_float(value)
    if value[0] == "+"
      on_unary("+", Node.new(tag: :float).add_token(value[1..]))
    else
      Node.new(tag: :float).add_token(value)
    end
  end

  def on_fndptn(const, presplat, values, postsplat)
    node = Node.new(tag: :find_pattern)
      .add_field(:class, const)
    node.add_child(splatparameter_var_field(presplat)) unless presplat.nil?
    node.add_children(values.map { |x| to_pattern x }) unless values.nil?
    node.add_child(splatparameter_var_field(postsplat)) unless postsplat.nil?
    node
  end

  def on_for(iterator, enumerable, stmts_add)
    if Node === iterator and iterator.tag == :var_field
      iterator = iterator.children[0]
    else
      iterator = Node.new(tag: :left_assignment_list).add_children([*iterator])
    end
    node = Node.new(tag: :for)
      .add_field(:pattern, iterator)
      .add_field(:value, Node.new(tag: :in).add_child(enumerable))
    node.add_field(:body, Node.new(tag: :do).add_children(stmts_add)) unless stmts_add.nil?
    node
  end

  def on_hash(assoclist_from_args)
    Node.new(tag: :hash).add_children(assoclist_from_args)
  end

  def in_heredoc()
    @string_stack[-1]&.start_with?("<")
  end

  def on_heredoc_beg(token)
    @string_stack.append(token)
    token
  end

  def on_heredoc_dedent(string_add, width)
    Node.new(tag: :heredoc_dedent)
  end

  def hashsplatparameter_var_field(var_field)
    if var_field.children[0].nil?
      Node.new(tag: :hash_splat_nil)
    else
      Node.new(tag: :hash_splat_parameter).add_field(:name, var_field.children[0])
    end
  end

  def on_hshptn(const, pairs, kwrest)
    node = Node.new(tag: :hash_pattern)
    node.add_field(:class, const)
    pairs.each do |k, v|
      keyvalue = Node.new(tag: :keyword_pattern)
      keyvalue.add_field(:key, k)
      keyvalue.add_field(:value, to_pattern(v)) unless v.nil?
      node.add_child(keyvalue)
    end unless pairs.nil?
    if kwrest.nil?
      if @kwrest
        node.add_child(Node.new(tag: :hash_splat_parameter))
      end
    else
      node.add_child(hashsplatparameter_var_field(kwrest))
    end
    @kwrest = false
    node
  end

  def on_imaginary(value)
    if value.end_with? "ri"
      child = on_rational(value[0...-1])
    else
      child = value.include?(".") ? on_float(value[0...-1]) : on_int(value[0...-1])
    end
    if child.tag == :unary
      on_unary(child.value, Node.new(tag: :complex).add_child(child.children[1]))
    else
      Node.new(tag: :complex).add_child(child)
    end
  end

  def on_ident(value)
    Node.new(tag: :identifier).add_token(value)
  end

  def on_label(value)
    Node.new(tag: :hash_key_symbol).add_token(value)
  end

  def on_if(predicate, then_stmts, else_stmts)
    node = Node.new(tag: :if)
    node.add_field(:condition, predicate)
    node.add_field(:consequence, Node.new(tag: :then).add_children(then_stmts)) unless then_stmts.empty?
    node.add_field(:alternative, else_stmts)
    node
  end

  def on_if_mod(predicate, statement)
    Node.new(tag: :if_modifier)
      .add_field(:body, statement)
      .add_field(:condition, predicate)
  end

  def on_ifop(predicate, truthy, falsy)
    Node.new(tag: :conditional)
      .add_field(:condition, predicate)
      .add_field(:consequence, truthy)
      .add_field(:alternative, falsy)
  end

  def on_in(pattern, stmts_add, consequent)
    if consequent.nil?
      if stmts_add.nil?
        return Node.new(tag: @oneline_pattern).add_field(:pattern, to_pattern(pattern))
      end
      case_match = Node.new(tag: :case_match)
    elsif consequent.tag == :else
      case_match = Node.new(tag: :case_match).add_field(:else, consequent)
    else
      case_match = consequent
    end
    in_clause = Node.new(tag: :in_clause)
    in_clause.add_field(:pattern, to_pattern(pattern))
    if pattern.tag == :unless_modifier
      in_clause.add_field(:guard, Node.new(tag: :unless_guard).add_field(:condition, pattern.children[-1]))
    elsif pattern.tag == :if_modifier
      in_clause.add_field(:guard, Node.new(tag: :if_guard).add_field(:condition, pattern.children[-1]))
    end
    in_clause.add_field(:body, Node.new(tag: :then).add_children([*stmts_add])) unless [*stmts_add].empty?
    case_match.prepend_field(:clauses, in_clause)
    case_match
  end

  def on_int(value)
    if value[0] == "+"
      on_unary("+", Node.new(tag: :integer).add_token(value[1..]))
    else
      Node.new(tag: :integer).add_token(value)
    end
  end

  def on_ivar(value)
    Node.new(tag: :instance_variable).add_token(value)
  end

  def on_gvar(value)
    Node.new(tag: :global_variable).add_token(value)
  end

  def on_backref(value)
    on_gvar(value)
  end

  def on_backtick(value)
    Node.new(tag: :operator).add_token(value)
  end

  def on_semicolon(value)
    @last_semicolon = [self.lineno, self.column]
  end

  def on_op(value)
    # Set @kwrest if the operator is **. The purpose is to detect the difference between
    # { k:, } and { k:, **} in on_hshptn() which receives a 'nil' value for the kwrest variable
    # in both cases
    @kwrest = value == "**"
    @oneline_pattern = :match_pattern if value == "=>"
    Node.new(tag: :operator).add_token(value)
  end

  def on_rparen(value)
    @last_rparen = { location: [self.lineno, self.column], used: false }
    nil
  end

  def on_kw(value)
    @oneline_pattern = :test_pattern if value == "in"
    if ["nil", "true", "false", "self"].include? value
      return Node.new(tag: :"#{value}")
    end
    if ["BEGIN", "END"].include? value
      return Node.new(tag: :constant).add_token(value)
    end
    # keyword identifiers
    return Node.new(tag: :identifier).add_token(value)
  end

  def on_kwrest_param(ident)
    Node.new(tag: :hash_splat_parameter).add_field(:name, ident)
  end

  def on_lambda(params, stmts)
    node = Node.new(tag: :lambda)
    if Array === params
      node.add_field(:parameters, Node.new(tag: :lambda_parameters).add_children(to_parameters(params))) unless params.empty?
    elsif params.tag == :parenthesized_statements
      node.add_field(:parameters, Node.new(tag: :lambda_parameters).add_children(to_parameters(params.children))) unless params.children.empty?
    end unless params.nil?
    node.add_field(:body, Node.new(tag: @lamdba_block_kind[0]).add_field(:body, Node.new(tag: @lamdba_block_kind[1]).add_children(stmts))) unless (stmts.nil? || stmts.empty?)
    node
  end

  def on_magic_comment(key, value)
  end

  def on_massign(left, right)
    if Array === right
      if right.length == 0
        right = nil
      elsif right.length == 1
        right = right[0]
      else
        right = Node.new(tag: :right_assignment_list).add_children(right)
      end
    end
    Node.new(tag: :assignment)
      .add_field(:left, Node.new(tag: :left_assignment_list).add_children([*left]))
      .add_field(:right, right)
  end

  def on_method_add_arg(method, arguments)
    if arguments.nil?
      method.add_field(:arguments, Node.new(tag: :argument_list))
    else
      arguments = [*arguments]
      method.add_field(:arguments, Node.new(tag: :argument_list).add_children(arguments)) unless arguments.empty?
    end
    method
  end

  def on_method_add_block(method, block)
    if block and [:break, :return, :next].include? method.tag
      call = method.children[0].children[0]
      call.add_field(:block, block)
      return method
    end
    if method.tag == :super
      method = Node.new(tag: :call).add_field(:method, method)
    end
    method.add_field(:block, block) if block
    method
  end

  def on_mlhs_add(mlhs, part)
    if part.tag == :var_field
      part = part.children[0]
    end
    mlhs.append(part)
    mlhs
  end

  def on_mlhs_add_post(mlhs_add_star, mlhs_add)
    mlhs_add_star.concat(mlhs_add)
    mlhs_add_star
  end

  def on_mlhs_add_star(mlhs, part)
    node = Node.new(tag: :rest_assignment)
    unless part.nil?
      if part.tag == :var_field
        part = part.children[0]
      end
      node.add_child(part)
    end
    mlhs.append(node)
    mlhs
  end

  def on_mlhs_new()
    []
  end

  def on_mlhs_paren(contents)
    Node.new(tag: :destructured_left_assignment).add_children([*contents])
  end

  def on_module(const, bodystmt)
    strip_empty(bodystmt)
    node = Node.new(tag: :module).add_field(:name, const)
    node.add_field(:body, Node.new(tag: :body_statement).add_children(bodystmt)) unless (bodystmt.nil? || bodystmt.empty?)
    node
  end

  def on_mrhs_add(mrhs, part)
    mrhs.children.append(part)
    mrhs
  end

  def on_mrhs_add_star(mrhs, part)
    mrhs.children.append(Node.new(tag: :splat_argument).add_child(part))
  end

  def on_mrhs_new()
    Node.new(tag: :right_assignment_list)
  end

  def on_mrhs_new_from_args(args)
    Node.new(tag: :right_assignment_list).add_children(args)
  end

  def on_next(arguments)
    return_like(:next, arguments)
  end

  def on_nokw_param(value)
    Node.new(tag: :hash_splat_nil)
  end

  def on_opassign(left, operator, right)
    if left.tag == :var_field
      left = left.children[0]
    end
    Node.new(tag: :operator_assignment)
      .add_field(:left, left)
      .add_token(operator)
      .add_field(:right, right)
  end

  def on_operator_ambiguous(operator, ambiguity)
    operator
  end

  def on_param_error(message, node)
    print "#{self.lineno}: #{message}\n"
    node
  end

  def on_params(req, opts, rest, post, keys, keyrest, block)
    args = []
    args.concat(req) unless req.nil?
    opts.each do |pair|
      args.append(Node.new(tag: :optional_parameter).add_field(:name, pair[0]).add_field(:value, pair[1]))
    end unless opts.nil?
    unless rest.nil?
      if rest.tag == :forward_argument
        args.append(Node.new(tag: :forward_parameter))
      else
        args.append(rest)
      end
    end
    args.concat(post) unless post.nil?
    keys.each do |pair|
      node = Node.new(tag: :keyword_parameter)
      node.add_field(:name, on_ident(pair[0]))
      node.add_field(:value, pair[1]) if pair[1]
      args.append(node)
    end unless keys.nil?
    unless keyrest.nil?
      if keyrest == :nil
        args.append(Node.new(tag: :hash_splat_nil))
      elsif keyrest.tag == :forward_argument
        args.append(Node.new(tag: :forward_parameter))
      else
        args.append(keyrest)
      end
    end
    args.append(block) unless block == :& or block.nil?
    args
  end

  def on_paren(contents)
    children = contents == false ? [] : [*contents]
    if !@last_rparen.nil? && @last_rparen[:location] == [self.lineno, self.column - 1] && @last_rparen[:used] && !children.empty?
      last_child = children[-1]
      if last_child.tag == :unary
        operand = last_child.children[1]
        if operand.tag == :parenthesized_statements
          last_child.children.pop
          last_child.add_field(:operand, operand.children[0])
        end
      end
    end
    Node.new(tag: :parenthesized_statements).add_children(children)
  end

  def on_parse_error(message)
    print "#{self.lineno}: #{message}\n"
    Node.new(tag: :error)
  end

  def on_program(stmts)
    node = Node.new(tag: :program)
    node.add_children(stmts)
    if @has_end
      node.add_child(Node.new(tag: :uninterpreted))
    end
    node
  end

  def on_qsymbols_add(qsymbols, tstring_content)
    qsymbols.children.append(Node.new(tag: :bare_symbol).add_children(string_content(tstring_content)))
    qsymbols
  end

  def on_qsymbols_beg(value)
    @string_stack.append(value)
  end

  def on_qsymbols_new()
    Node.new(tag: :symbol_array)
  end

  def on_qwords_add(qwords, tstring_content)
    qwords.children.append(Node.new(tag: :bare_string).add_children(string_content(tstring_content)))
    qwords
  end

  def on_qwords_beg(value)
    @string_stack.append(value)
  end

  def on_qwords_new()
    Node.new(tag: :string_array)
  end

  def on_rational(value)
    child = value.include?(".") ? on_float(value[0...-1]) : on_int(value[0...-1])
    if child.tag == :unary
      on_unary(child.value, Node.new(tag: :rational).add_child(child.children[1]))
    else
      Node.new(tag: :rational).add_child(child)
    end
  end

  def on_redo()
    Node.new(tag: :redo)
  end

  def on_regexp_add(regexp, part)
    if String === part
      regexp.concat(string_content(part))
    else
      regexp.append(part)
    end
  end

  def on_regexp_literal(regexp, ending)
    @string_stack.pop()
    Node.new(tag: :regex).add_children(regexp)
  end

  def on_regexp_beg(value)
    @string_stack.append(value)
  end

  def on_regexp_new()
    []
  end

  def on_rescue(exceptions, variable, stmts_add, consequent)
    clause = Node.new(tag: :rescue)
    unless exceptions.nil?
      if Node === exceptions and exceptions.tag == :right_assignment_list
        exceptions = exceptions.children
      end
      clause.add_field(:exceptions, Node.new(tag: :exceptions).add_children([*exceptions]))
    end
    unless variable.nil?
      if variable.tag == :var_field
        variable = variable.children[0]
      end
      clause.add_field(:variable, Node.new(tag: :exception_variable).add_child(variable))
    end
    clause.add_field(:body, Node.new(tag: :then).add_children(stmts_add)) unless stmts_add.nil? or stmts_add.empty?
    if consequent.nil?
      consequent = []
    end
    consequent.prepend(clause)
    consequent
  end

  def on_rescue_mod(statement, rescued)
    Node.new(tag: :rescue_modifier)
      .add_field(:body, statement)
      .add_field(:handler, rescued)
  end

  def on_rest_param(ident)
    Node.new(tag: :splat_parameter).add_field(:name, ident)
  end

  def on_retry()
    Node.new(tag: :retry)
  end

  def on_return(arguments)
    return_like(:return, arguments)
  end

  def on_return0()
    Node.new(tag: :return)
  end

  def on_sclass(object, bodystmt)
    strip_empty(bodystmt)
    node = Node.new(tag: :singleton_class)
      .add_field(:value, object)
    node.add_field(:body, Node.new(tag: :body_statement).add_children(bodystmt)) unless (bodystmt.nil? || bodystmt.empty?)
    node
  end

  def on_stmts_add(stmts, stmt)
    @lamdba_block_kind = [:block, :block_body]
    if Array === stmts
      stmts.append(stmt) unless stmt.nil?
    else
      stmts.children.append(stmt) unless stmt.nil?
    end
    stmts
  end

  def on_stmts_new()
    return []
  end

  def on_string_add(string, part)
    if String === part
      string.add_children(string_content(part))
    else
      string.add_child(part)
    end
    string
  end

  def on_string_concat(left, right)
    if left.tag == :chained_string
      left.add_child(right)
      left
    else
      Node.new(tag: :chained_string).add_child(left).add_child(right)
    end
  end

  def on_string_content()
    if in_heredoc()
      Node.new(tag: :heredoc_body)
    elsif @string_stack[-1] == ":\"" || @string_stack[-1] == ":'" || @string_stack[-1].start_with?("%s")
      Node.new(tag: :delimited_symbol)
    else
      Node.new(tag: :string)
    end
  end

  def on_string_dvar(variable)
    on_string_embexpr([variable])
  end

  def on_string_embexpr(stmts_add)
    Node.new(tag: :interpolation).add_children(stmts_add)
  end

  def on_string_literal(parts)
    if parts.tag == :heredoc_body
      heredoc_start = Node.new(tag: :heredoc_beginning)
      heredoc_end = Node.new(tag: :heredoc_end)
      parts.children.each do |p|
        if p.tag == :string_content
          p.tag = :heredoc_content
        end
      end
      parts.add_child(heredoc_end)
    end
    @string_stack.pop()
    parts
  end

  def on_super(args)
    args = [*args]

    on_command(Node.new(tag: :super), args.empty? ? nil : args)
  end

  def on_symbol(contents)
    @string_stack.pop()
    Node.new(tag: :simple_symbol).add_token(contents)
  end

  def on_symbol_literal(contents)
    unless Node === contents
      raise "on_symbol_literal: #{contents}"
    end
    contents
  end

  def on_symbols_add(qsymbols, word_add)
    qsymbols.children.append(Node.new(tag: :bare_symbol).add_children(word_add))
    qsymbols
  end

  def on_symbols_beg(value)
    @string_stack.append(value)
  end

  def on_symbols_new()
    Node.new(tag: :symbol_array)
  end

  def on_top_const_field(const)
    on_top_const_ref(const)
  end

  def on_top_const_ref(const)
    Node.new(tag: :scope_resolution).add_field(:name, const)
  end

  def on_tstring_content(value)
    value
  end

  def on_unary(operator, value)
    if (operator == :not || operator == :"defined?") && !@last_rparen.nil? && @last_rparen[:location] == [self.lineno, self.column - 1]
      @last_rparen[:used] = true
      value = Node.new(tag: :parenthesized_statements).add_child(value)
    end

    Node.new(tag: :unary).add_token(operator).add_field(:operand, value)
  end

  def method_name(node)
    case node.tag
    when :simple_symbol, :operator, :delimited_symbol, :constant, :global_variable
      node
    when :identifier
      if node.value.end_with? "="
        Node.new(tag: :setter).add_field(:name, on_ident(node.value[0...-1]))
      else
        if /[A-Z]/.match?(node.value[0])
           node.tag = :constant
        end  
        node
      end
    else
      node.tag = :identifier
      node
    end
  end

  def on_undef(methods)
    Node.new(tag: :undef).add_children(methods.map { |x| method_name x })
  end

  def on_unless(predicate, stmts_add, consequent)
    node = Node.new(tag: :unless)
    node.add_field(:condition, predicate)
    node.add_field(:consequence, Node.new(tag: :then).add_children(stmts_add)) unless stmts_add.empty?
    node.add_field(:alternative, consequent)
    node
  end

  def on_unless_mod(predicate, statement)
    Node.new(tag: :unless_modifier)
      .add_field(:body, statement)
      .add_field(:condition, predicate)
  end

  def on_until(predicate, stmts_add)
    Node.new(tag: :until)
      .add_field(:condition, predicate)
      .add_field(:body, Node.new(tag: :do).add_children(stmts_add))
  end

  def on_until_mod(predicate, statement)
    Node.new(tag: :until_modifier)
      .add_field(:body, statement)
      .add_field(:condition, predicate)
  end

  def on_var_alias(left, right)
    on_alias(left, right)
  end

  def on_var_field(ident)
    if ident == :nil
      return Node.new(tag: :var_field)
    end
    Node.new(tag: :var_field).add_field(:name, ident)
  end

  def on_var_ref(ident)
    ident
  end

  def on_vcall(ident)
    ident
  end

  def on_void_stmt()
    if @last_semicolon == [self.lineno, self.column - 1]
      Node.new(tag: :empty_statement)
    else
      nil
    end
  end

  def on_when(predicate, stmts_add, consequent)
    if consequent.nil?
      case_ = Node.new(tag: :case)
    elsif consequent.tag == :else
      case_ = Node.new(tag: :case).add_child(consequent)
    else
      case_ = consequent
    end
    clause = Node.new(tag: :when)
    predicate.each { |p| clause.add_field(:pattern, Node.new(tag: :pattern).add_child(p)) }
    clause.add_field(:body, Node.new(tag: :then).add_children(stmts_add)) unless stmts_add.empty?
    case_.prepend_child(clause)
    case_
  end

  def on_while(predicate, stmts_add)
    Node.new(tag: :while)
      .add_field(:condition, predicate)
      .add_field(:body, Node.new(tag: :do).add_children(stmts_add))
  end

  def on_while_mod(predicate, statement)
    Node.new(tag: :while_modifier)
      .add_field(:body, statement)
      .add_field(:condition, predicate)
  end

  def on_word_add(word, part)
    word.concat(string_content(part))
  end

  def on_word_new()
    []
  end

  def on_words_add(words, word_add)
    words.children.append(Node.new(tag: :bare_string).add_children(word_add))
    words
  end

  def on_words_new()
    Node.new(tag: :string_array)
  end

  def on_words_beg(value)
    @string_stack.append(value)
  end

  def on_xstring_add(xstring, part)
    if String === part
      xstring.add_children(string_content(part))
    else
      xstring.add_child(part)
    end
    xstring
  end

  def on_xstring_literal(parts)
    if parts.tag == :heredoc_body
      heredoc_start = Node.new(tag: :heredoc_beginning)
      heredoc_end = Node.new(tag: :heredoc_end)
      parts.children.each do |p|
        if p.tag == :string_content
          p.tag = :heredoc_content
        end
      end
      parts.add_child(heredoc_end)
    end
    @string_stack.pop()
    parts
  end

  def on_xstring_new()
    if in_heredoc()
      Node.new(tag: :heredoc_body)
    else
      @string_stack.append(token)
      Node.new(tag: :subshell)
    end
  end

  def on_yield(arguments)
    if Node === arguments and arguments.tag == :parenthesized_statements
      arguments = arguments.children
    end
    node = Node.new(tag: :yield)
    node.add_child(Node.new(tag: :argument_list).add_children(arguments)) unless arguments.nil?
    node
  end

  def on_yield0()
    Node.new(tag: :yield)
  end

  def on_zsuper()
    Node.new(tag: :super)
  end

  def on_tstring_beg(value)
    @string_stack.append(value)
  end

  def on_symbeg(value)
    @string_stack.append(value)
  end

  def on___end__(value)
    @has_end = true
  end

  def string_content(text)
    if not String === text
      return [text]
    end
    if not @string_stack[-1]&.start_with?("%r") and
       not @string_stack[-1]&.start_with?("%Q") and
       not @string_stack[-1]&.start_with?("%'") and
      (@string_stack[-1].to_s.include? ("'") or @string_stack[-1]&.start_with?("%q") or
       @string_stack[-1]&.start_with?("%w") or @string_stack[-1]&.start_with?("%i"))
      # ideally the contents of regexp nodes would also be treated as raw strings
      # or @string_stack[-1] == "/" or @string_stack[-1]&.start_with?("%r")
      return [Node.new(tag: :string_content).add_token(text)]
    end
    escape = /\\([^ux0-7]|x[0-9a-fA-F]{1,2}|[0-7]{1,3}|u[0-9a-fA-F]{4}|u{[0-9a-fA-F ]+})/
    pos = 0
    result = []
    while m = escape.match(text, pos)
      if pos < m.begin(0)
        result.append(Node.new(tag: :string_content).add_token(text[pos..(m.begin(0) - 1)]))
      end
      result.append(Node.new(tag: :escape_sequence).add_token(m[0]))
      pos = m.end(0)
    end
    if pos < text.length
      result.append(Node.new(tag: :string_content).add_token(text[pos..text.length]))
    end
    result
  end

  def to_pattern(tree)
    case tree.tag
    when :var_field
      return tree.children[0]
    when :begin
      node = Node.new(tag: :expression_reference_pattern)
      tree.children.each { |c| node.add_field(:value, c) }
      return node
    when :unless_modifier, :if_modifier
      return to_pattern(tree.children[0])
    when :global_variable, :instance_variable, :class_variable
      return Node.new(tag: :variable_reference_pattern).add_field(:name, tree)
    when :identifier
      case tree.value
      when "__LINE__" then return Node.new(tag: :line)
      when "__FILE__" then return Node.new(tag: :file)
      when "__ENCODING__" then return Node.new(tag: :encoding)
      else
        return Node.new(tag: :variable_reference_pattern).add_field(:name, tree)
      end
    when :binary
      left = to_pattern(tree.children[0])
      right = to_pattern(tree.children[-1])

      if tree.children[1].tag == :"|"
        if left.tag == :alternative_pattern
          node = left
        else
          node = Node.new(tag: :alternative_pattern).add_field(:alternatives, left)
        end
        node.add_field(:alternatives, right)
        return node
      elsif tree.children[1].tag == :"=>"
        return Node.new(tag: :as_pattern).add_field(:value, left).add_field(:name, right)
      end
    end
    return tree
  end
end

def process_heredocs(tree, docs = [])
  if tree.nil? || tree.field.nil?
    return docs
  end
  case tree.tag
  when :heredoc_body
    prev_tag = nil
    doc = Node.new(tag: :heredoc_body)
    tree.children.each { |x|
      cur_tag = x.tag
      if prev_tag.nil? and cur_tag != :heredoc_content
        doc.add_child(Node.new(tag: :heredoc_content))
      end
      unless prev_tag == :heredoc_content and cur_tag == :heredoc_content
        doc.add_child(x)
      end
      prev_tag = cur_tag
    }
    tree.children.clear()
    tree.tag = :heredoc_beginning
    docs.append(doc)
  when :program, :method, :do_block, :do, :singleton_method, :class, :module, :singleton_class, :block, :begin, :else, :elsif, :then
    children = []
    children.concat(docs)
    tree.children.each { |t| children.append(t); children.concat(process_heredocs(t, [])) }
    tree.children.clear
    tree.children.concat(children)
    []
  else
    tree.children.each { |t|
      docs = process_heredocs(t, docs)
    }
    docs
  end
end

if ARGV.length != 1
  puts "Usage: ruby #{File.basename(__FILE__)} FILE"
  exit
end
path = ARGV[0]
# pp Ripper.sexp_raw(File.open(path), path)
tree = AST.new(File.open(path), path).parse
# pp tree
process_heredocs(tree)
tree.pp unless tree.nil?
print "\n"
