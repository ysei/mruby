assert('Thread.new') do
  t = Thread.new do
    'test'
  end
  assert_false t.nil?
  assert_equal('test') { t.join }
end

assert('Thread.join') do
  t = Thread.new do
    # return nil when no expression is evaluated.
  end
  assert_nil t.join
end

#
# Exception is currently not supported.
#
#assert('Thread.join with exception') do
#  t = Thread.new do
#    raise 'error'
#  end
#  assert_false t.join.nil?
#end

assert('multiple threads') do
  t1 = Thread.new do
    i = 0
    100.times do
      i = i + 1
    end
    i
  end
  t2 = Thread.new do
    i = 0
    200.times do
      i = i + 1
    end
  end
  assert_equal(100) { t1.join }
  assert_equal(200) { t2.join }
end

assert('arg') do
  n = 1
  t1 = Thread.new(n) do |x|
    10.times do
      x = x + 1
    end
    x
  end
  n = 11
  t2 = Thread.new(n) do |x|
    10.times do
      x = x + 1
    end
    x
  end
  assert_equal(11) { t1.join }
  assert_equal(21) { t2.join }
end
