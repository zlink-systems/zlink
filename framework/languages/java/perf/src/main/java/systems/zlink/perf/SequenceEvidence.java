package systems.zlink.perf;

import java.util.*;

/** Exact numeric membership; compact ranges are emitted only outside the measured window. */
public final class SequenceEvidence {
    private static final class Stream {
        final Set<Long> attempts=new HashSet<>(),windowAdmission=new HashSet<>(),settleAdmission=new HashSet<>(),windowReceipt=new HashSet<>(),settleReceipt=new HashSet<>();
        long duplicates;
    }
    private final Map<Integer,Stream> streams=new TreeMap<>();
    private Stream stream(int id){return streams.computeIfAbsent(id,k->new Stream());}
    public void attempt(int id,String sequence){stream(id).attempts.add(Long.parseUnsignedLong(sequence));}
    public void admission(int id,String sequence,boolean window){(window?stream(id).windowAdmission:stream(id).settleAdmission).add(Long.parseUnsignedLong(sequence));}
    public boolean receipt(int id,String sequence,boolean window){
        var s=stream(id);long seq=Long.parseUnsignedLong(sequence);
        if(s.windowReceipt.contains(seq)||s.settleReceipt.contains(seq)){s.duplicates++;return false;}
        (window?s.windowReceipt:s.settleReceipt).add(seq);return true;
    }
    public void clear(){streams.clear();}
    public long unique(){return streams.values().stream().mapToLong(s->s.windowReceipt.size()+s.settleReceipt.size()).sum();}
    public List<Object> sources(){var rows=new ArrayList<Object>();streams.forEach((id,s)->rows.add(Measurement.map("clientId",id,"attemptedRanges",ranges(s.attempts),"windowAdmissionRanges",ranges(s.windowAdmission),"settleAdmissionRanges",ranges(s.settleAdmission))));return rows;}
    public List<Object> receivers(){var rows=new ArrayList<Object>();streams.forEach((id,s)->rows.add(Measurement.map("clientId",id,"windowRanges",ranges(s.windowReceipt),"settleRanges",ranges(s.settleReceipt),"duplicateCount",Long.toString(s.duplicates))));return rows;}
    public Map<String,Object> publisher(){var s=stream(0);return Measurement.map("attemptedRanges",ranges(s.attempts),"windowSuccessRanges",ranges(s.windowAdmission),"settleSuccessRanges",ranges(s.settleAdmission));}
    public Map<String,Object> subscriber(int id){var s=stream(0);return Measurement.map("subscriberId",id,"windowRanges",ranges(s.windowReceipt),"settleRanges",ranges(s.settleReceipt),"duplicateEvents",Long.toString(s.duplicates),"timingEvidence",null,"nullReasons",Measurement.map("/timingEvidence",Measurement.reason("CLOCK_DOMAIN_UNVERIFIED","Separate process clocks are not aligned")));}
    public static List<Object> ranges(Set<Long> values){
        var sorted=new ArrayList<>(values);sorted.sort(Long::compareUnsigned);var ranges=new ArrayList<Object>();long first=0,last=0;boolean have=false;
        for(long value:sorted){if(!have){first=last=value;have=true;}else if(last!=-1&&value==last+1)last=value;else{ranges.add(Measurement.map("first",Long.toUnsignedString(first),"last",Long.toUnsignedString(last)));first=last=value;}}
        if(have)ranges.add(Measurement.map("first",Long.toUnsignedString(first),"last",Long.toUnsignedString(last)));return ranges;
    }
}
