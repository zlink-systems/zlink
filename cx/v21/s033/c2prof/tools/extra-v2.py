import profile as p
for mode in ['alloc','alloc-stacks','threadstat']:
 for rep in ([1,2] if mode=='threadstat' else [1]):
  for pattern in ['request-backpressure','send-saturation']:
   for level in ([1,2] if rep==1 else [2,1]):p.run(level,pattern,rep,mode)
