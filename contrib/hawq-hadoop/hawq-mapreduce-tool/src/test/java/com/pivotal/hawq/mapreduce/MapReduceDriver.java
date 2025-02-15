package com.pivotal.hawq.mapreduce;

import org.apache.hadoop.conf.Configured;
import org.apache.hadoop.io.Text;
import org.apache.hadoop.mapreduce.Mapper;
import org.apache.hadoop.mapreduce.Reducer;
import org.apache.hadoop.util.Tool;

import java.io.IOException;

/**
 * Base class for mapreduce driver.
 */
abstract class MapReduceDriver extends Configured implements Tool {

	// Map HAWQRecord to record string, which consists of fields seperated by '|'.
	static class HAWQTableMapper extends Mapper<Void, HAWQRecord, Text, Text> {
		@Override
		protected void map(Void key, HAWQRecord value, Context context)
				throws IOException, InterruptedException {
			try {
				String recordString = toRecordString(value);
				context.write(new Text(recordString), new Text(recordString));

			} catch (HAWQException e) {
				throw new IOException(e);
			}
		}

		private String toRecordString(HAWQRecord record) throws HAWQException {
			StringBuilder buf = new StringBuilder(toFieldString(record, 1));
			for (int i = 2; i <= record.getSchema().getFieldCount(); i++) {
				buf.append("|").append(toFieldString(record, i));
			}
			return buf.toString();
		}

		private String toFieldString(HAWQRecord record, int fieldIndex)
				throws HAWQException {
			Object val = record.getObject(fieldIndex);
			if (val == null) return "null";
			if (val instanceof byte[]) return new String((byte[]) val);
			return val.toString();
		}
	}

	static class HAWQTableReducer extends Reducer<Text, Text, Text, Text> {
		@Override
		protected void reduce(Text key, Iterable<Text> values, Context context)
				throws IOException, InterruptedException {
			for (Text value : values) {
				context.write(null, new Text(value));
			}
		}
	}
}
