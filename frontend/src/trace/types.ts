export interface FragmentTrace { id:number; name:string; size:number }
export interface MismatchDetail { overlap_offset:number; left_byte:number; right_byte:number }
export interface EdgeTrace { from:number; to:number; overlap:number; matches:number; mismatches:number; exact:boolean; mismatch_details:MismatchDetail[] }
export interface RepairTrace { position:number; stage:'consensus'|'png_crc'; before:number; after:number; support:number; observations:number; candidates_tested:number; crc_consistent:boolean }
export interface Trace {
  schema_version:1
  engine:{name:string;version:string}
  configuration:{min_overlap:number;max_mismatches:number;graph_build:string;threads:number;strategy:string;beam_width:number;io:string;format:string;repair:string}
  fragments:FragmentTrace[]; edges:EdgeTrace[]; selected_path:number[]
  reconstruction:{strategy:string;complete:boolean;fragments_used:number;fragments_total:number;output_bytes:number;approximate_joins:number;overlap_mismatches:number}
  mismatches:Array<MismatchDetail & {from:number;to:number}>; repairs:RepairTrace[]; unresolved_ambiguities:number
  format:{type:string;signature_valid:boolean;structurally_valid:boolean;width:number|null;height:number|null;valid_crc_count:number;invalid_crc_count:number;all_crc_valid:boolean;chunks:unknown[]}
  graph_stats:{strategy:string;theoretical_pairs:number;candidate_pairs:number;full_overlap_checks:number;edges_created:number;threads_used:number}
}
