module Outer
  #    ^ definition.module
  VERSION = "1"
  # <- definition.constant
  class Foo < Base
    #   ^ definition.class
    #         ^ reference.class
    def self.build; end
    #        ^ definition.method
    def name=(v); end
    #   ^ definition.method
    alias other name
    #     ^ definition.method
  end
end
require "set"
helper(1)
# <- reference.call
