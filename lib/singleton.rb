#  Singleton module that ensures only one object to be allocated.
#
# Usage:
#   class SomeSingletonClass
#     include Singleton
#    #....
#   end
#   a = SomeSingletonClass.instance
#   b = SomeSingletonClass.instance	# a and b are same object
#   p [a,b]
#   a = SomeSingletonClass.new		# error (`new' is private)

module Singleton
  def Singleton.append_features(klass)
    klass.private_class_method(:new)
    klass.instance_eval %{
      def instance
	unless @__instance__
	  @__instance__ = new
	end
	return @__instance__
      end
    }
  end
end

if __FILE__ == $0
  class SomeSingletonClass
    include Singleton
    #....
  end

  a = SomeSingletonClass.instance
  b = SomeSingletonClass.instance	# a and b are same object
  p [a,b]
  a = SomeSingletonClass.new		# error (`new' is private)
end
