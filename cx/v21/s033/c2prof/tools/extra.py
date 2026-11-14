import profile as p
for mode in ['stat','alloc','alloc-stacks','trace']:
 for pattern in ['request-backpressure','send-saturation']:
  for level in [1,2]:p.run(level,pattern,1,mode)
