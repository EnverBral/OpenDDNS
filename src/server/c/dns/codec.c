/**
 * Functions to interpret and process DNS questions.
 * It reads through the buffer of a given size and binds it to a DNS message.
 */

#include "codec.h"

/**
 * Read all next labels from the buffer until a null byte is encountered (the null will also be
 * taken out of the buffer).
 * This can be used for labels in questions and resource records.
 * The amount of labels read will be stored in 'labels_size'.
 */
dnsmsg_label_t* interpret_labels(unsigned char** buffer, ssize_t* buffer_size, uint16_t* labels_size) {
    uint8_t next_size;
    dnsmsg_label_t* labels;
    unsigned int amount, i;
    amount = 0;

    while ((next_size = pop_int8(buffer, buffer_size)) > 0) {
        if(!amount) {
            labels = malloc(sizeof(dnsmsg_label_t));
        } else {
            labels = realloc(labels, sizeof(dnsmsg_label_t) * (amount + 1));
        }
        labels[amount].name_size = next_size;
        labels[amount].name = calloc(sizeof(uint8_t), labels[amount].name_size);
        for(i = 0; i < labels[amount].name_size; i++) {
            labels[amount].name[i] = pop_int8(buffer, buffer_size);
        }
        amount++;
    }

    *labels_size = amount;

    return labels;
}

/**
 * Convert a buffer to a DNS message.
 * The error flag will be set to 1 if no ip address can be found for the question domain.
 */
dnsmsg_t interpret_question(unsigned char* buffer, ssize_t buffer_size, int* error_flag) {
    unsigned int i, j;
    dnsmsg_t message;

    message.header.id = pop_int16(&buffer, &buffer_size);
    message.header.status_flags = pop_int16(&buffer, &buffer_size);
    message.header.query_count = pop_int16(&buffer, &buffer_size);
    message.header.answer_count = pop_int16(&buffer, &buffer_size);
    message.header.authority_count = pop_int16(&buffer, &buffer_size);
    message.header.additional_count = pop_int16(&buffer, &buffer_size);

    message.questions = malloc(sizeof(dnsmsg_question_t) * message.header.query_count);
    for(i = 0; i < message.header.query_count; i++) {
        message.questions[i].labels = interpret_labels(&buffer, &buffer_size,
                &message.questions[i].labels_size);
        message.questions[i].type = pop_int16(&buffer, &buffer_size);
        message.questions[i].class = pop_int16(&buffer, &buffer_size);
    }

    message.answers = interpret_rr(message.header.answer_count, &buffer, &buffer_size);
    message.authorities = interpret_rr(message.header.authority_count, &buffer, &buffer_size);
    message.additionals = interpret_rr(message.header.additional_count, &buffer, &buffer_size);

    if(buffer_size < 0) {
        fprintf(stderr, "Read invalid packet, ignoring. (tried reading outside of buffer size)\n");
        *error_flag = 1;
    }

    return message;
}

/**
 * Read from a buffer to make a given amount of resource records.
 * This will reduce the buffer_size by the amount of bytes read from the buffer.
 * The buffer will also be reduced to the part without the read resource records.
 */
dnsmsg_rr_t* interpret_rr(uint16_t amount, unsigned char** buffer, ssize_t* buffer_size) {
    int i, j;
    dnsmsg_rr_t* rr;

    rr = malloc(sizeof(dnsmsg_rr_t) * amount);
    for(i = 0; i < amount; i++) {
        rr[i].labels = interpret_labels(buffer, buffer_size, &rr[i].labels_size);
        rr[i].type = pop_int16(buffer, buffer_size);
        rr[i].class = pop_int16(buffer, buffer_size);
        rr[i].ttl = pop_int32(buffer, buffer_size);
        rr[i].data_size = pop_int16(buffer, buffer_size);
        rr[i].data = malloc(sizeof(uint8_t) * rr[i].data_size);
        for(j = 0; j < rr[i].data_size; j++) {
            rr[i].data[j] = pop_int8(buffer, buffer_size);
        }
    }

    return rr;
}

/**
 * Take the first 8 bits from the start of the buffer.
 * The buffer_size will then also be reduced by 1 byte.
 */
uint8_t pop_int8(unsigned char** buffer, ssize_t* buffer_size) {
    uint8_t value = 0;
    (*buffer_size)--;
    if(*buffer_size >= 0) {
        value = *((*buffer)++);
    }
    return value;
}

/**
 * Take the first 16 bits from the start of the buffer.
 * The buffer_size will then also be reduced by 2 bytes.
 */
uint16_t pop_int16(unsigned char** buffer, ssize_t* buffer_size) {
    return pop_int8(buffer, buffer_size) << 8 | pop_int8(buffer, buffer_size);
}

/**
 * Take the first 32 bits from the start of the buffer.
 * The buffer_size will then also be reduced by 4 bytes.
 */
uint32_t pop_int32(unsigned char** buffer, ssize_t* buffer_size) {
    return pop_int16(buffer, buffer_size) << 16 | pop_int16(buffer, buffer_size);
}

/**
 * Serialize a given message to a buffer and also store the size of the constructed buffer.
 * This function will allocate the required space itself.
 * This won't do any checks for whether the buffer size exceeds the maximum datagram size
 * for DNS packets, that's your own damn responsibility!
 */
void serialize_message(dnsmsg_t message, unsigned char** buffer, ssize_t* buffer_size) {
    // The index will be incremented by one every place byte in the buffer.
    unsigned int i, j, index;
    *buffer_size = calc_message_size(message);
    *buffer = malloc(*buffer_size);

    // Go through the message to place it inside the buffer.
    index = 0;
    append_int16(buffer, &index, message.header.id);
    append_int16(buffer, &index, message.header.status_flags);
    append_int16(buffer, &index, message.header.query_count);
    append_int16(buffer, &index, message.header.answer_count);
    append_int16(buffer, &index, message.header.authority_count);
    append_int16(buffer, &index, message.header.additional_count);

    for(i = 0; i < message.header.query_count; i++) {
        append_labels(buffer, &index, message.questions[i].labels, message.questions[i].labels_size);
        append_int16(buffer, &index, message.questions[i].type);
        append_int16(buffer, &index, message.questions[i].class);
    }

    append_resource_records(buffer, &index, message.answers, message.header.answer_count);
    append_resource_records(buffer, &index, message.authorities, message.header.authority_count);
    append_resource_records(buffer, &index, message.additionals, message.header.additional_count);
}

/**
 * Calculate the size required to store the given labels.
 */
size_t calc_labels_size(dnsmsg_label_t* labels, uint16_t labels_size) {
    unsigned int i;
    size_t size = 1; // For the mandatory last null byte.

    for(i = 0; i < labels_size; i++) {
        size += sizeof(labels[i].name_size);
        size += labels[i].name_size * sizeof(uint8_t);
    }

    return size;
}

/**
 * Calculate the full size of a message.
 * Meaning that dynamic arrays will also be followed and taken into account
 * for the size calculation.
 */
size_t calc_message_size(dnsmsg_t message) {
    unsigned int i;
    size_t size = sizeof(dnsmsg_header_t);

    // Referenced parts of question
    for(i = 0; i < message.header.query_count; i++) {
        size += calc_labels_size(message.questions[i].labels, message.questions[i].labels_size);
        size += sizeof(message.questions[i].type);
        size += sizeof(message.questions[i].class);
    }

    size += calc_resource_records_size(message.answers, message.header.answer_count);
    size += calc_resource_records_size(message.authorities, message.header.authority_count);
    size += calc_resource_records_size(message.additionals, message.header.additional_count);

    return size;
}

/**
 * Calculate the full size of a list of resource records.
 */
size_t calc_resource_records_size(dnsmsg_rr_t* resource_records, uint16_t amount) {
    unsigned int i;
    size_t size = 0;

    for(i = 0; i < amount; i++) {
        size += calc_labels_size(resource_records[i].labels, resource_records[i].labels_size);
        size += sizeof(resource_records[i].type);
        size += sizeof(resource_records[i].class);
        size += sizeof(resource_records[i].ttl);
        size += sizeof(resource_records[i].data_size);
        size += resource_records[i].data_size * sizeof(uint8_t);
    }

    return size;
}

/**
 * Append 8 bits to the buffer and increment the index by one.
 */
void append_int8(unsigned char** buffer, unsigned int* index, uint8_t value) {
    (*buffer)[(*index)++] = value;
}

/**
 * Append 16 bits to the buffer and increment the index by two.
 */
void append_int16(unsigned char** buffer, unsigned int* index, uint16_t value) {
    append_int8(buffer, index, value >> 8);
    append_int8(buffer, index, value & 255);
}

/**
 * Append 32 bits to the buffer and increment the index by four.
 */
void append_int32(unsigned char** buffer, unsigned int* index, uint32_t value) {
    append_int16(buffer, index, value >> 16);
    append_int16(buffer, index, value & 65535);
}

/**
 * Append labels to the buffer.
 */
void append_labels(unsigned char** buffer, unsigned int* index, dnsmsg_label_t* labels,
        uint16_t labels_size) {
    unsigned int i, j;

    for(i = 0; i < labels_size; i++) {
        append_int8(buffer, index, labels[i].name_size);
        for(j = 0; j < labels[i].name_size; j++) {
            append_int8(buffer, index, labels[i].name[j]);
        }
    }

    // Always end with a null byte
    append_int8(buffer, index, 0);
}

/**
 * Add a full list of resource records to the buffer and increment the index accordingly.
 */
void append_resource_records(unsigned char** buffer, unsigned int* index, dnsmsg_rr_t* resource_records,
        uint16_t amount) {
    unsigned int i, j;
    for(i = 0; i < amount; i++) {
        append_labels(buffer, index, resource_records[i].labels, resource_records[i].labels_size);
        append_int16(buffer, index, resource_records[i].type);
        append_int16(buffer, index, resource_records[i].class);
        append_int32(buffer, index, resource_records[i].ttl);
        append_int16(buffer, index, resource_records[i].data_size);
        for(j = 0; j < resource_records[i].data_size; j++) {
            append_int8(buffer, index, resource_records[i].data[j]);
        }
    }
}

/**
 * Encode header status flags into 16 bits.
 * QR: 1 bit; 0 if query, 1 if response.
 * OPCODE: 4 bits: 0 if query, 1 if inverse query, 2 if status request
 * AA: 1 bit: (only valid in responses) 1 if this is an authoritive answer
 * TC: 1 bit: if this message (ID) is truncated, should be 0 on the last of truncated message parts.
 * RD: 1 bit: if recursion is required, this bit is copied from query to response if the recursion
 * was denied.
 * RA: 1 bit: (only valid in responses) if this server can accept recursive requests.
 * RCODE: 4 bits: see errorcodes in codes.h
 */
uint16_t encode_status_flags(int qr, int opcode, int aa, int tc, int rd, int ra, int rcode) {
    int z = 0; // Must always be zero
    return qr << 15 | opcode << 11 | aa << 10 | tc << 9 | rd << 8 | ra << 7 | z << 4 | rcode;
}

/**
 * Decode 16 bits header status flags.
 * QR: 1 bit; 0 if query, 1 if response.
 * OPCODE: 4 bits: 0 if query, 1 if inverse query, 2 if status request
 * AA: 1 bit: (only valid in responses) 1 if this is an authoritive answer
 * TC: 1 bit: if this message (ID) is truncated, should be 0 on the last of truncated message parts.
 * RD: 1 bit: if recursion is required, this bit is copied from query to response if the recursion
 * was denied.
 * RA: 1 bit: (only valid in responses) if this server can accept recursive requests.
 * RCODE: 4 bits: see errorcodes in codes.h
 */
void decode_status_flags(uint16_t status_flags , int* qr, int* opcode, int* aa, int* tc, int* rd, int* ra, int* rcode) {
    *qr = (status_flags >> 15) & 1;
    *opcode = (status_flags >> 11) & 15;
    *aa = (status_flags >> 10) & 1;
    *tc = (status_flags >> 9) & 1;
    *rd = (status_flags >> 8) & 1;
    *ra = (status_flags >> 7) & 1;
    // status_flags >> 4 & 7; Would be the Z.
    *rcode = status_flags & 15;
}

/**
 * Check if this message is a trunctated one.
 */
int is_truncated(dnsmsg_t message) {
    return (message.header.status_flags >> 9) & 1;
}
