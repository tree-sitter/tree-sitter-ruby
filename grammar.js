const PREC = {
  AND: -1,
  OR: -1,
  NOT: 5,
  DEFINED: 10,
  ASSIGN: 15,
  CONDITIONAL: 20,
  RANGE: 25,
  BOOLEAN_OR: 30,
  BOOLEAN_AND: 35,
  RELATIONAL: 40,
  COMPARISON: 45,
  BITWISE_OR: 50,
  BITWISE_AND: 55,
  SHIFT: 60,
  ADDITIVE: 65,
  MULTIPLICATIVE: 70,
  UNARY_MINUS: 75,
  EXPONENTIAL: 80,
  COMPLEMENT: 85,
};

const unbalancedDelimiters = '!@#$%^&*)]}>|\\=/+-~`\'",.?:;_';

module.exports = grammar({
  name: 'ruby',

  extras: $ => [
    $.comment,
    $._line_break,
    /[ \t\r]/
  ],

  rules: {
    program: $ => sep($._statement, $._terminator),

    _statement: $ => choice(
      $._declaration,
      seq($._call, "do", optional("|", commaSep($._block_variable), "|"), sep($._statement, $._terminator), "end"),
      seq("undef", $._function_name),
      seq("alias", $._function_name, $._function_name),
      $.while_statement,
      $.until_statement,
      $.if_statement,
      $.unless_statement,
      $.for_statement,
      $.begin_statement,
      $.return_statement,
      $.if_modifier,
      $.unless_modifier,
      $.while_modifier,
      $.until_modifier,
      $._expression
    ),

    _declaration: $ => choice(
      $.method_declaration,
      $.class_declaration,
      $.module_declaration
    ),

    method_declaration: $ => seq(
      "def", $._function_name, choice(seq("(", optional($.argument_list), ")"), seq(optional($.argument_list), $._terminator)),
      sep($._statement, $._terminator),
      "end"
    ),

    argument_list: $ => commaSep1(seq(optional(choice("*", "&")), $.identifier)),

    class_declaration: $ => seq("class", $.identifier, optional(seq("<", sep1($.identifier, "::"))), $._terminator, sep($._statement, $._terminator), "end"),

    module_declaration: $ => seq("module", $.identifier, $._terminator, sep($._statement, $._terminator), "end"),

    while_statement: $ => seq("while", $.condition, $._statement_block),
    until_statement: $ => seq("until", $.condition, $._statement_block),
    if_statement: $ => seq("if", $.condition, $._then_elsif_else_block),
    unless_statement: $ => seq("unless", $.condition, $._then_else_block),
    for_statement: $ => seq("for", $._lhs, "in", $._expression, $._statement_block),
    begin_statement: $ => seq(
      "begin",
      sep($._statement, $._terminator),
      repeat($.rescue_block),
      optional($.else_block),
      optional($.ensure_block),
      "end"
    ),

    return_statement: $ => seq("return", optional($._expression)),

    if_modifier: $ => seq($._statement, "if", $._expression),
    unless_modifier: $ => seq($._statement, "unless", $._expression),
    while_modifier: $ => seq($._statement, "while", $._expression),
    until_modifier: $ => seq($._statement, "until", $._expression),

    condition: $ => $._expression,

    _statement_block: $ => choice(
      $._do_block,
      seq($._terminator, sep($._statement, $._terminator), "end")
    ),
    _do_block: $ => seq("do", sep($._statement, $._terminator), "end"),

    then_block: $ => seq(choice("then", $._terminator), sep($._statement, $._terminator)),
    else_block: $ => seq("else", sep($._statement, $._terminator)),
    rescue_block: $ => seq("rescue", commaSep($._argument), choice("do", $._terminator), sep($._statement, $._terminator)),
    ensure_block: $ => seq("ensure", sep($._statement, $._terminator)),

    _then_else_block: $ => seq($.then_block, optional($.else_block), "end"),
    _then_elsif_else_block: $ => seq(
      $.then_block,
      repeat(seq(
        "elsif", $.condition,
        $.then_block
      )),
      optional($.else_block),
      "end"
    ),

    _call: $ => choice($._function_call, $._command),

    _call_arguments: $ => choice(
      commaSep1($._argument),
      $._command
    ),

    _command: $ => choice(
      seq("super", $._call_arguments)
    ),
    _function_call: $ => choice("super"),

    _expression: $ => choice(
      $._argument,
      $.yield,
      $.and,
      $.or,
      $.not,
      $.defined,
      $.assignment,
      $.conditional,
      $.range,
      $.boolean_or,
      $.boolean_and,
      $.relational,
      $.comparison,
      $.bitwise_or,
      $.bitwise_and,
      $.shift,
      $.additive,
      $.multiplicative,
      $.unary_minus,
      $.exponential,
      $.complement,
      $._literal
    ),

    _argument: $ => choice($._primary),

    _primary: $ => choice(
      seq("(", sep($._statement, $._terminator), ")"),
      $._lhs
    ),

    scope_resolution_expression: $ => seq(optional($._primary), '::', $.identifier),
    subscript_expression: $ => seq($._primary, "[", commaSep($._argument), "]"),
    member_access: $ => seq($._primary, ".", $.identifier),
    yield: $ => seq("yield", optional($._expression)),

    and: $ => prec.left(PREC.AND, seq($._expression, "and", $._expression)),
    or: $ => prec.left(PREC.OR, seq($._expression, "or", $._expression)),
    not: $ => prec.right(PREC.NOT, seq("not", $._expression)),
    defined: $ => prec(PREC.DEFINED, seq("defined?", $._expression)),
    assignment: $ => prec.right(PREC.ASSIGN, seq($._lhs, '=', $._expression)),
    conditional: $ => prec.right(PREC.CONDITIONAL, seq($._expression, '?', $._expression, ':', $._expression)),
    range: $ => prec.right(PREC.RANGE, seq($._expression, choice('..', '...'), $._expression)),

    boolean_or: $ => prec.left(PREC.BOOLEAN_OR, seq($._expression, '||', $._expression)),
    boolean_and: $ => prec.left(PREC.BOOLEAN_OR, seq($._expression, '&&', $._expression)),

    relational: $ => prec.right(PREC.RELATIONAL, seq($._expression, choice('==', '!=', '===', '<=>', '=~', '!~'), $._expression)),
    comparison: $ => prec.left(PREC.COMPARISON, seq($._expression, choice('<', '<=', '>', '>='), $._expression)),

    bitwise_or: $ => prec.left(PREC.BITWISE_OR, seq($._expression, choice('^', '|'), $._expression)),
    bitwise_and: $ => prec.left(PREC.BITWISE_AND, seq($._expression, '&', $._expression)),
    shift: $ => prec.left(PREC.SHIFT, seq($._expression, choice('<<', '>>'), $._expression)),

    additive: $ => prec.left(PREC.ADDITIVE, seq($._expression, choice('-', '+'), $._expression)),
    multiplicative: $ => prec.left(PREC.MULTIPLICATIVE, seq($._expression, choice('*', '/', '%'), $._expression)),

    unary_minus: $ => prec.right(PREC.UNARY_MINUS, seq('-', $._expression)),

    exponential: $ => prec.right(PREC.EXPONENTIAL, seq($._expression, '**', $._expression)),

    complement: $ => prec.right(PREC.COMPLEMENT, seq(choice('!', '~', '+'), $._expression)),

    _block_variable: $ => choice($._lhs, $._mlhs),
    _mlhs: $ => choice(
      seq(commaSep1($._mlhs_item), optional(seq("*", optional($._lhs)))),
      seq("*", optional($._lhs))
    ),
    _mlhs_item: $ => choice($._lhs, seq("(", $._mlhs, ")")),
    _lhs: $ => choice(
      $._variable,
      $.scope_resolution_expression,
      $.subscript_expression,
      $.member_access
    ),
    _variable: $ => choice($.identifier , 'self'),

    identifier: $ => token(seq(repeat(choice('@', '$')), identifierChars())),

    comment: $ => token(choice(
      seq('#', /.*/),
      seq(
        '=begin\n',
        repeat(/.*\n/),
        '=end\n'
      )
    )),

    _literal: $ => choice(
      $.symbol,
      $.integer,
      $.float,
      $.boolean,
      $.nil,
      $.string,
      $.subshell,
      $.array
    ),

    symbol: $ => choice(
      token(seq(':', choice(identifierChars(), operatorChars())))
      // seq(':"', $._double_quoted_body),
      // seq(":'", $._single_quoted_body)
    ),
    integer: $ => (/0b[01](_?[01])*|0[oO]?[0-7](_?[0-7])*|(0d)?\d(_?\d)*|0x[0-9a-fA-F](_?[0-9a-fA-F])*/),
    float: $ => (/\d(_?\d)*\.\d(_?\d)*([eE]\d(_?\d)*)?/),
    boolean: $ => choice('true', 'false', 'TRUE', 'FALSE'),
    nil: $ => choice('nil', 'NIL'),

    string: $ => choice(
      $._single_quoted,
      $._double_quoted,
      $._percent,
      $._percent_q
    ),
    _single_quoted: $ => stringBody('', "'"),
    _double_quoted: $ => stringBody('', '"', $.interpolation),
    _percent: $ => choice(
      seq(/%Q?/, choice.apply(null, unbalancedDelimiters.split('').map(d => seq(stringBody(/%Q?/, d, $.interpolation))))),
      seq(/%Q?</, $._percent_angle, '>'),
      seq(/%Q?\{/, $._percent_bracket, '}'),
      seq(/%Q?\(/, $._percent_paren, ')'),
      seq(/%Q?\[/, $._percent_brace, ']')
    ),
    _percent_angle: $ => balancedInner($._percent_angle, '<', '>', $.interpolation),
    _percent_bracket: $ => balancedInner($._percent_bracket, '[', ']', $.interpolation),
    _percent_paren: $ => balancedInner($._percent_paren, '(', ')', $.interpolation),
    _percent_brace: $ => balancedInner($._percent_brace, '{', '}', $.interpolation),
    _percent_q: $ => choice(
      choice.apply(null, unbalancedDelimiters.split('').map(d => seq(stringBody('%q', d)))),
      seq('%q<', $._percent_q_angle, '>'),
      seq('%q[', $._percent_q_bracket, ']'),
      seq('%q(', $._percent_q_paren, ')'),
      seq('%q{', $._percent_q_brace, '}')
    ),
    _percent_q_angle: $ => balancedInner($._percent_q_angle, '<', '>'),
    _percent_q_bracket: $ => balancedInner($._percent_q_bracket, '[', ']'),
    _percent_q_paren: $ => balancedInner($._percent_q_paren, '(', ')'),
    _percent_q_brace: $ => balancedInner($._percent_q_brace, '{', '}'),
    interpolation: $ => seq(token(prec(20, '#{')), $._expression, '}'),

    subshell: $ => choice(
      $._backticks
    ),
    _backticks: $ => stringBody('', '`'),

    array: $ => seq('[', $._array_items, ']'),
    _array_items: $ => optional(seq($._expression, optional(seq(',', $._array_items)))),

    _function_name: $ => choice($.identifier, operatorChars()),

    _line_break: $ => '\n',
    _terminator: $ => choice($._line_break, ';'),
  }
});

/// Describes the body of a string literal bounded by `delimiter`, and optionally containing (potentially recursive) references to `insert`.
function stringBody (leading, delimiter, insert) {
  if (typeof insert === 'undefined') {
    return seq(leading, delimiter, repeat(choice(/\\./, RegExp('[^\\\\\\' + delimiter + ']'))), delimiter);
  } else {
    return seq(leading, delimiter, repeat(choice(/\\./, insert, RegExp('[^\\\\\\' + delimiter + ']'))), delimiter);
  }
}

function balancedInner (me, open, close, insert) {
  if (typeof insert === 'undefined') {
    return repeat(choice(/\\./, seq(open, me, close), RegExp('[^\\\\\\' + open + '\\' + close + ']')));
  } else {
    return repeat(choice(/\\./, seq(open, me, close), insert, RegExp('[^\\\\\\' + open + '\\' + close + ']')));
  }
}

function balancedStringBody (me, open, close, insert) {
  if (typeof insert === 'undefined') {
    return seq(open, repeat(choice(/\\./, me, RegExp('[^\\\\\\' + open + '\\' + close + ']'))), close);
  } else {
    return seq(open, repeat(choice(/\\./, me, insert, RegExp('[^\\\\\\' + open + '\\' + close + ']'))), close);
  }
}

function identifierChars () {
  return /[a-zA-Z_][a-zA-Z0-9_]*/;
}

function operatorChars () {
  return choice('..', '|', '^', '&', '<=>', '==', '===', '=~', '>', '>=', '<', '<=', '+', '-', '*', '/', '%', '**', '<<', '>>', '~', '+@', '-@', '[]', '[]=');
}

function sep1 (rule, separator) {
  return seq(rule, repeat(seq(separator, rule)));
}

function sep (rule, separator) {
  return optional(sep1(rule, separator));
}

function commaSep1 (rule) {
  return sep1(rule, ',')
}

function commaSep (rule) {
  return optional(commaSep1(rule));
}
