#
# $Id$
#
# The class for temporary files.
#  o creates a temporary file, which name is "basename.pid.n" with mode "w+".
#  o Tempfile objects can be used like IO object.
#  o with tmpfile.close(true) created temporary files are removed.
#  o created files are also removed on script termination.
#  o with Tempfile#open, you can reopen the temporary file.
#  o file mode of the temporary files are 0600.

require 'delegate'
require 'final'

class Tempfile < SimpleDelegator
  Max_try = 10

  def Tempfile.callback(path)
    lambda{
      print "removing ", path, "..." if $DEBUG
      if File.exist?(path)
	File.unlink(path) 
      end
      if File.exist?(path + '.lock')
	Dir.rmdir(path + '.lock')
      end
      print "done\n" if $DEBUG
    }
  end

  def initialize(basename, tmpdir=ENV['TMPDIR']||ENV['TMP']||ENV['TEMP']||'/tmp')
    umask = File.umask(0177)
    begin
      n = 0
      while true
	begin
	  tmpname = sprintf('%s/%s%d.%d', tmpdir, basename, $$, n)
	  lock = tmpname + '.lock'
	  unless File.exist?(tmpname) or File.exist?(lock)
	    Dir.mkdir(lock)
	    break
	  end
	rescue
	  raise "cannot generate tmpfile `%s'" % tmpname if n >= Max_try
	  #sleep(1)
	end
	n += 1
      end

      @clean_files = Tempfile.callback(tmpname)
      ObjectSpace.define_finalizer(self, @clean_files)

      @tmpfile = File.open(tmpname, 'w+')
      @tmpname = tmpname
      super(@tmpfile)
      Dir.rmdir(lock)
    ensure
      File.umask(umask)
    end
  end

  def Tempfile.open(*args)
    Tempfile.new(*args)
  end

  def open
    @tmpfile.close if @tmpfile
    @tmpfile = File.open(@tmpname, 'r+')
    __setobj__(@tmpfile)
  end

  def close(real=false)
    @tmpfile.close if @tmpfile
    @tmpfile = nil
    if real
      @clean_files.call
      ObjectSpace.undefine_finalizer(self)
    end
  end

  def path
    @tmpname
  end
end

if __FILE__ == $0
#  $DEBUG = true
  f = Tempfile.new("foo")
  f.print("foo\n")
  f.close
  f.open
  p f.gets # => "foo\n"
  f.close(true)
end
