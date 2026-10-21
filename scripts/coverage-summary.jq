# Exact Clopper-Pearson binomial intervals by inversion of the binomial CDF.
# NIST: https://www.itl.nist.gov/div898/handbook/prc/section2/prc241.htm
# Log-space recurrence prevents underflow in the binomial sum; 50 bisections.
def logadd($a;$b):
 if $a==null then $b else ([$a,$b]|max) as $m | $m + ((($a-$m)|exp) + (($b-$m)|exp)|log) end;
def logcdf($k;$n;$p):
 ($p|log) as $lp | ((1-$p)|log) as $lq |
 reduce range(0;$k+1) as $j ({term:($n*$lq), sum:null};
  .sum=logadd(.sum;.term) |
  if $j<$k then .term += ((($n-$j)/($j+1))|log)+$lp-$lq else . end) | .sum;
def upper($k;$n;$tail):
 if $k==$n then 1 elif $k==0 then 1-($tail|pow(.;1/$n)) else
  ($tail|log) as $target |
  reduce range(0;50) as $i ({lo:0,hi:1};
   ((.lo+.hi)/2) as $mid |
   if logcdf($k;$n;$mid)>$target then .lo=$mid else .hi=$mid end) | (.lo+.hi)/2 end;
def ci($k;$n;$alpha):
 if $n==0 then null else {low:(1-upper($n-$k;$n;$alpha/2)),high:upper($k;$n;$alpha/2)} end;
def ci95($k;$n): ci($k;$n;0.05);
def summarize($comparisons):
 ([.[]|select(.outcome=="codec_rejection" or .outcome=="framing_rejection")]|length) as $rejected |
 ([.[]|select(.changed)]|length) as $changed |
 {trials:length,changed:$changed,rejected:$rejected,
  codec_rejections:([.[]|select(.outcome=="codec_rejection")]|length),
  framing_rejections:([.[]|select(.outcome=="framing_rejection")]|length),
  accepted:([.[]|select(.outcome=="accepted")]|length),
  noops:([.[]|select(.outcome=="noop")]|length),
  unchanged:([.[]|select(.outcome=="unchanged")]|length),
  crashes:([.[]|select(.outcome=="crash")]|length),
  timeouts:([.[]|select(.outcome=="timeout")]|length),
  limits:([.[]|select(.outcome=="limit")]|length),
  errors:([.[]|select(.outcome=="error")]|length),
  rate:(if $changed==0 then null else $rejected/$changed end),
  ci95:ci95($rejected;$changed),
  simultaneous_ci95:ci($rejected;$changed;0.05/$comparisons),
  complete:all(.[];.outcome=="codec_rejection" or .outcome=="framing_rejection" or .outcome=="accepted" or .outcome=="noop" or .outcome=="unchanged")};
