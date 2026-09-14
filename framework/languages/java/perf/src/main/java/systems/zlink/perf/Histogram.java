package systems.zlink.perf;

import java.math.BigInteger;
import java.util.*;

public final class Histogram {
    public static final long[] BOUNDS;
    static {
        var values = new ArrayList<Long>();
        long value=1000;
        for (;;) { values.add(value); if(value==60_000_000_000L)break; value=Math.min(60_000_000_000L,(value*101+99)/100); }
        BOUNDS=values.stream().mapToLong(Long::longValue).toArray();
    }
    private final long[] buckets=new long[BOUNDS.length];
    private long count,overflow,max;
    private BigInteger sum=BigInteger.ZERO;
    public void record(long ns) {
        if(ns<0)throw new IllegalArgumentException("Negative monotonic duration");
        int index=Arrays.binarySearch(BOUNDS,ns); if(index<0)index=-index-1;
        if(index==BOUNDS.length)overflow++;else buckets[index]++;
        count++;sum=sum.add(BigInteger.valueOf(ns));max=Math.max(max,ns);
    }
    public Map<String,Object> snapshot() {
        return Measurement.map("unit","ms","ticksUnit","ns","bucketSpec","ns-1us-1pct-60s-v1",
            "boundsNs",Arrays.stream(BOUNDS).mapToObj(Long::toString).toList(),
            "counts",Arrays.stream(buckets).mapToObj(Long::toString).toList(),"overflow",Long.toString(overflow),
            "count",Long.toString(count),"sumNs",sum.toString(),"maxNs",count==0?null:Long.toString(max),
            "percentileMethod","nearest-rank-bucket-upper-bound");
    }
    public Double percentile(int thousandths) {
        long rank=BigInteger.valueOf(count).multiply(BigInteger.valueOf(thousandths)).add(BigInteger.valueOf(999)).divide(BigInteger.valueOf(1000)).longValueExact();
        long cumulative=0;
        for(int i=0;i<buckets.length;i++){ cumulative+=buckets[i]; if(cumulative>=rank)return BOUNDS[i]/1e6; }
        return null;
    }
    public void export(String prefix,String key,Map<String,Object> metrics,Map<String,Object> histograms,Map<String,Object> reasons) {
        histograms.put(key,snapshot());reasons.remove("/histograms/"+key);
        for(String suffix:List.of("meanMs","p50Ms","p95Ms","p99Ms","p999Ms","maxMs")) {
            boolean insufficient=suffix.equals("p999Ms")&&count<100000;
            Double value=count==0||insufficient?null:switch(suffix){
                case "meanMs"->sum.doubleValue()/count/1e6; case "maxMs"->max/1e6;
                case "p50Ms"->percentile(500);case "p95Ms"->percentile(950);case "p99Ms"->percentile(990);default->percentile(999);};
            String path="/metrics/"+prefix+"."+suffix;
            metrics.put(prefix+"."+suffix,value);reasons.remove(path);
            if(value==null){var reason=Measurement.reason(insufficient?"INSUFFICIENT_SAMPLES":count==0?"NO_SAMPLES":"HISTOGRAM_OVERFLOW","No publishable percentile sample");if(count>0&&!insufficient)reason.put("lowerBoundMs",60000);reasons.put(path,reason);}
        }
        if(count==0)reasons.put("/histograms/"+key+"/maxNs",Measurement.reason("NO_SAMPLES","No samples"));
    }
}
