#
# How to use:
#
# db = PStore.new("/tmp/foo")
# db.transaction do
#   p db.roots
#   ary = db["root"] = [1,2,3,4]
#   ary[0] = [1,1.5]
# end

# db.transaction do
#   p db["root"]
# end

require "marshal"

class PStore
  class Error < StandardError
  end

  def initialize(file)
    dir = File::dirname(file)
    unless File::directory? dir
      raise PStore::Error, format("directory %s does not exist", dir)
    end
    unless File::writable? dir
      raise PStore::Error, format("directory %s not writable", dir)
    end
    if File::exist? file and not File::readable? file
      raise PStore::Error, format("file %s not readable", file)
    end
    @transaction = false
    @filename = file
    @abort = false
  end

  def in_transaction
    raise PStore::Error, "not in transaction" unless @transaction
  end
  private :in_transaction

  def [](name)
    in_transaction
    value = @table[name]
    if value == nil
      raise PStore::Error, format("undefined root name `%s'", name)
    end
    value
  end
  def []=(name, value)
    in_transaction
    @table[name] = value
  end

  def roots
    in_transaction
    @table.keys
  end
  def root?(name)
    in_transaction
    @table.key? name
  end
  def path
    @filename
  end

  def commit
    @abort = false
    throw :pstore_abort_transaction
  end
  def abort
    @abort = true
    throw :pstore_abort_transaction
  end

  def transaction
    raise PStore::Error, "nested transaction" if @transaction
    begin
      @transaction = true
      value = file = nil
      begin
	File::open(@filename, "r") do |file|
	  @table = Marshal.load(file)
	end
      rescue Errno::ENOENT
	@table = {}
      end
      begin
	catch(:pstore_abort_transaction) do
	  value = yield(self)
	end
      rescue Exception
	@abort = true
	raise
      ensure
	unless @abort
	  begin
	    File::rename @filename, @filename+"~"
	  rescue Errno::ENOENT
	    no_orig = true
	  end
	  begin
	    File::open(@filename, "w") do |file|
	      Marshal::dump(@table, file)
	    end
	  rescue
	    File::rename @filename+"~", @filename unless no_orig
	  end
	end
	@abort = false
      end
    ensure
      @table = nil
      @transaction = false
    end
    value
  end
end

if __FILE__ == $0
  db = PStore.new("/tmp/foo")
  db.transaction do
    p db.roots
    ary = db["root"] = [1,2,3,4]
    ary[1] = [1,1.5]
  end

  1000.times do
    db.transaction do
      db["root"][0] += 1
      p db["root"][0]
    end
  end
end
